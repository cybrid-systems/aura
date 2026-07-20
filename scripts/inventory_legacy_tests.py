#!/usr/bin/env python3
"""Issue #1957: inventory legacy test_issue_*.cpp (+ root issue-oriented tests).

Scans tests/issues/test_issue_*.cpp and root-level tests/test_*.cpp that look
issue-oriented, extracts theme signals from the filename + first 50 lines, and
writes a living inventory for domain/ migration planning.

Usage:
  python3 scripts/inventory_legacy_tests.py           # write inventory
  python3 scripts/inventory_legacy_tests.py --check   # fail if stale
  python3 scripts/inventory_legacy_tests.py --json path.json  # also dump JSON

Output (default): tests/legacy_test_inventory.md
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass, field
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
ISSUES = TESTS / "issues"
DEFAULT_OUT = TESTS / "legacy_test_inventory.md"

# ── Theme buckets (8) — aligned with #1957 examples ─────────────────────────
# Priority for scoring ties: earlier themes win when scores are equal after
# secondary sort (see classify()). Order also drives the migration roadmap.
THEME_ORDER = [
    "arena_compaction",
    "mutation_dirty",
    "fiber_orch",
    "linear_ownership",
    "edsl_hygiene",
    "jit_incremental",
    "shape_soa",
    "observability",
]

THEME_META: dict[str, dict[str, str]] = {
    "arena_compaction": {
        "title": "Arena / compaction / GC",
        "target": "tests/domain/ (extend compact/gc family; see test_compact_*_batch)",
        "priority": "P0 — well-contained, batch drivers already exist",
    },
    "mutation_dirty": {
        "title": "Mutation / dirty propagation / provenance",
        "target": "tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch",
        "priority": "P0 — high volume; strong domain suite foothold",
    },
    "fiber_orch": {
        "title": "Fiber / orchestration / steal / Guard",
        "target": "tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch",
        "priority": "P1 — domain suite already collapses many obs gates",
    },
    "linear_ownership": {
        "title": "Linear ownership / borrow / consume",
        "target": "tests/test_linear_ownership_batch.cpp → domain/",
        "priority": "P1 — small, already partially batched",
    },
    "edsl_hygiene": {
        "title": "EDSL / macro hygiene / reflect",
        "target": "tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect batch",
        "priority": "P1 — domain hygiene suite exists",
    },
    "jit_incremental": {
        "title": "JIT / AOT / incremental relower",
        "target": "domain suite for incremental_*; keep heavy JIT in issue bundles",
        "priority": "P2 — link-profile heavy; migrate AC smoke first",
    },
    "shape_soa": {
        "title": "Shape / SoA / column layout",
        "target": "tests/test_soa_batch.cpp → domain/",
        "priority": "P2 — small-medium; soa_batch precedent",
    },
    "observability": {
        "title": "Observability / metrics / query:*-stats",
        "target": "tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp",
        "priority": "P2 — often thin schema probes; collapse into obs matrix",
    },
    "uncategorized": {
        "title": "Uncategorized / mixed",
        "target": "manual triage before domain placement",
        "priority": "P3 — review case-by-case",
    },
}

# Keyword regexes applied to "filename + first 50 lines". Counts are additive.
THEME_PATTERNS: dict[str, list[str]] = {
    "arena_compaction": [
        r"\barena\b",
        r"\bcompact",
        r"\bdefrag",
        r"\bgc\b",
        r"\bquota\b",
        r"monotonic",
        r"run_destructors",
        r"\bepoch\b",
        r"ast.?arena",
        r"env.?arena",
        r"auto.?compact",
    ],
    "mutation_dirty": [
        r"\bmutat",
        r"dirty_propag",
        r"\bdirty\b",
        r"post_invalidate",
        r"bump_steal",
        r"atomic.?batch",
        r"mutation.?bound",
        r"occ_cache",
        r"\binvalidate",
        r"stable.?ref",
        r"provenance",
        r"post.?mutate",
        r"typed.?mutate",
        r"mutation.?log",
    ],
    "fiber_orch": [
        r"\bfiber\b",
        r"\bschedule",
        r"\bsteal\b",
        r"\byield\b",
        r"\bresume\b",
        r"\borchest",
        r"safepoint",
        r"\bworker\b",
        r"\bguard\b",
        r"fiber.?join",
        r"multi.?fiber",
    ],
    "linear_ownership": [
        r"\blinear\b",
        r"\bown(?:er|ership)\b",
        r"\bmove.?only\b",
        r"\bborrow\b",
        r"\bconsume\b",
        r"type.?driven.?linear",
        r"env.?bind.?linear",
        r"linear.?safety",
        r"linear.?types",
    ],
    "edsl_hygiene": [
        r"\bhygiene\b",
        r"\bmacro\b",
        r"\bedsl\b",
        r"\breflect\b",
        r"source.?marker",
        r"macro.?inline",
        r"\bquote\b",
        r"\bsyntax\b",
        r"macro.?expans",
        r"ir.?hygiene",
    ],
    "jit_incremental": [
        r"\bjit\b",
        r"\baot\b",
        r"\bincremental\b",
        r"\brelower\b",
        r"hot.?update",
        r"\bspec.?jit\b",
        r"\bllvm\b",
        r"pass.?manager",
        r"optimization.?pass",
        r"aura_jit",
        r"hotupdate",
    ],
    "shape_soa": [
        r"\bsoa\b",
        r"\bshape\b",
        r"\bcolumn\b",
        r"ir_soa",
        r"envframe.?soa",
        r"flat.?ast",
        r"shape.?profil",
        r"column.?compact",
    ],
    "observability": [
        r"\bobservab",
        r"engine:metrics",
        r"query:\S*-stats",
        r"\bmetrics\b",
        r"compiler.?metrics",
        r"primitive.?surface",
        r"@category:\s*integration",  # weak signal only
        r"schema\s*==",
        r"hash-ref.*stats",
    ],
}

THEME_COMPILED = {theme: [re.compile(p, re.IGNORECASE) for p in pats] for theme, pats in THEME_PATTERNS.items()}

# Filename token boosts (stronger than body keyword noise).
FILENAME_BOOSTS: dict[str, list[str]] = {
    "arena_compaction": ["arena", "compact", "defrag", "gc", "quota", "epoch"],
    "mutation_dirty": [
        "mutat",
        "dirty",
        "invalidate",
        "provenance",
        "atomic_batch",
        "stable_ref",
        "steal",  # steal often mutation-boundary; score both via body
    ],
    "fiber_orch": ["fiber", "orch", "steal", "resume", "safepoint", "scheduler", "guard"],
    "linear_ownership": ["linear", "ownership", "borrow", "consume"],
    "edsl_hygiene": ["hygiene", "macro", "edsl", "reflect", "marker"],
    "jit_incremental": ["jit", "aot", "incremental", "relower", "hot_update", "hotupdate", "llvm"],
    "shape_soa": ["soa", "shape", "column"],
    "observability": ["observability", "metrics", "stats", "closed_loop", "closedloop"],
}

CAT_RE = re.compile(r"^//\s*@category:\s*(\S+)", re.M)
REASON_RE = re.compile(r"^//\s*@reason:\s*(.+)$", re.M)
ISSUE_NUM_RE = re.compile(r"test_issue_(\d+)")
ROOT_ISSUE_NUM_RE = re.compile(r"(?:^test_|_)(\d{3,})")
INCLUDE_RE = re.compile(r'#include\s+"([^"]+)"')
IMPORT_RE = re.compile(r"^\s*import\s+([\w.]+)", re.M)
SUMMARY_RE = re.compile(
    r"^//\s*(?:test_\S+\s*[—-]\s*)?(.+)$",
)

HEAD_LINES = 50
# Root tests without digits that are clearly domain/batch (not "issue-oriented").
ROOT_SKIP_PREFIXES = (
    "test_harness",
    "test_concurrent",  # kept as long-lived stress, not issue legacy
)


@dataclass
class TestEntry:
    rel: str
    name: str
    location: str  # "issues" | "root"
    theme: str
    scores: dict[str, int]
    issue: int | None
    category: str
    reason: str
    summary: str
    size_bytes: int
    harness: list[str] = field(default_factory=list)
    includes: list[str] = field(default_factory=list)
    imports: list[str] = field(default_factory=list)
    flags: list[str] = field(default_factory=list)


def is_issue_oriented_root(path: Path) -> bool:
    """Root tests/test_*.cpp that are issue-oriented (numbers / issue / batch)."""
    name = path.name
    if not name.startswith("test_") or not name.endswith(".cpp"):
        return False
    for pref in ROOT_SKIP_PREFIXES:
        if name.startswith(pref):
            return False
    if "issue" in name:
        return True
    if re.search(r"\d{3,}", name):
        return True
    # Family batch drivers are migration milestones (inventory them).
    return "_batch" in name or name.startswith("test_domain_")


def extract_issue(path: Path) -> int | None:
    m = ISSUE_NUM_RE.search(path.name)
    if m:
        return int(m.group(1))
    # Prefer trailing _NNNN before suffix, else first long digit run.
    m2 = re.search(r"_(\d{3,})(?:_|\.cpp$)", path.name)
    if m2:
        return int(m2.group(1))
    m3 = ROOT_ISSUE_NUM_RE.search(path.name)
    return int(m3.group(1)) if m3 else None


def first_summary(lines: list[str]) -> str:
    """Best-effort one-line description from header comments."""
    for line in lines[:25]:
        s = line.strip()
        if not s.startswith("//"):
            continue
        body = s[2:].strip()
        if not body or body.startswith("@") or body.startswith("=="):
            continue
        if body.startswith("#include") or body.startswith("This binary"):
            continue
        # Prefer lines that mention Issue or describe purpose.
        if re.search(r"Issue\s*#?\d+|AC\d|verif|contract|closed.?loop|test_", body, re.I):
            return body[:160]
    for line in lines[:15]:
        s = line.strip()
        if s.startswith("//") and len(s) > 10:
            body = s[2:].strip()
            if body and not body.startswith("@"):
                return body[:160]
    return ""


def harness_flags(text: str) -> list[str]:
    flags: list[str] = []
    if "test_harness.hpp" in text:
        flags.append("test_harness")
    if "issue_test_harness" in text:
        flags.append("issue_test_harness")
    if "AURA_ISSUE_TEST" in text:
        flags.append("AURA_ISSUE_TEST")
    if "CompilerService" in text:
        flags.append("CompilerService")
    if "RUN_ALL_TESTS" in text:
        flags.append("RUN_ALL_TESTS")
    if re.search(r"\bint\s+main\s*\(", text):
        flags.append("own_main")
    if "aura_issue_" in text and "_run" in text:
        flags.append("bundle_run_fn")
    return flags


def score_theme(name: str, head: str) -> dict[str, int]:
    blob = f"{name}\n{head}"
    scores = {t: 0 for t in THEME_ORDER}
    for theme, pats in THEME_COMPILED.items():
        for pat in pats:
            scores[theme] += len(pat.findall(blob))
    # Filename boosts
    lower = name.lower()
    for theme, tokens in FILENAME_BOOSTS.items():
        for tok in tokens:
            if tok in lower:
                scores[theme] += 3
    return scores


def classify(scores: dict[str, int]) -> str:
    best = max(THEME_ORDER, key=lambda t: (scores.get(t, 0), -THEME_ORDER.index(t)))
    if scores.get(best, 0) <= 0:
        return "uncategorized"
    return best


def entry_flags(path: Path, size: int, issue: int | None) -> list[str]:
    flags: list[str] = []
    if size < 4000:
        flags.append("small")
    if size > 25000:
        flags.append("large")
    if "_phase" in path.name:
        flags.append("phase_slice")
    if "followup" in path.name:
        flags.append("followup")
    if "_batch" in path.name:
        flags.append("batch_driver")
    if path.parent.name == "domain" or path.name.startswith("test_domain_"):
        flags.append("domain_suite")
    if "minimal" in path.name:
        flags.append("minimal")
    if "observability" in path.name:
        flags.append("obs_named")
    if issue is not None and issue < 200:
        flags.append("early_issue")
    return flags


def scan_file(path: Path, location: str) -> TestEntry:
    raw = path.read_text(encoding="utf-8", errors="replace")
    lines = raw.splitlines()
    head_lines = lines[:HEAD_LINES]
    head = "\n".join(head_lines)
    scores = score_theme(path.name, head)
    theme = classify(scores)
    cat_m = CAT_RE.search(head)
    reason_m = REASON_RE.search(head)
    issue = extract_issue(path)
    includes = INCLUDE_RE.findall(head)
    imports = IMPORT_RE.findall(head)
    size = path.stat().st_size
    rel = path.relative_to(ROOT).as_posix()
    return TestEntry(
        rel=rel,
        name=path.name,
        location=location,
        theme=theme,
        scores={k: v for k, v in scores.items() if v > 0},
        issue=issue,
        category=cat_m.group(1) if cat_m else "unknown",
        reason=(reason_m.group(1).strip() if reason_m else "")[:120],
        summary=first_summary(head_lines),
        size_bytes=size,
        harness=harness_flags(head + "\n" + "\n".join(lines[HEAD_LINES : HEAD_LINES + 80])),
        includes=includes[:12],
        imports=imports[:12],
        flags=entry_flags(path, size, issue),
    )


def collect() -> list[TestEntry]:
    entries: list[TestEntry] = []
    if ISSUES.is_dir():
        for p in sorted(ISSUES.glob("test_issue_*.cpp")):
            entries.append(scan_file(p, "issues"))
    for p in sorted(TESTS.glob("test_*.cpp")):
        if is_issue_oriented_root(p):
            entries.append(scan_file(p, "root"))
    # Domain suites + theme pilots (e.g. domain/arena/, #1959).
    domain = TESTS / "domain"
    if domain.is_dir():
        domain_files = list(domain.glob("test_*.cpp")) + list(domain.glob("*/test_*.cpp"))
        for p in sorted(set(domain_files)):
            e = scan_file(p, "domain")
            if "domain_suite" not in e.flags:
                e.flags.append("domain_suite")
            if p.parent != domain:
                e.flags.append(f"theme_{p.parent.name}")
            entries.append(e)
    return entries


def multi_issue_groups(entries: list[TestEntry]) -> dict[int, list[str]]:
    by: dict[int, list[str]] = defaultdict(list)
    for e in entries:
        if e.location == "issues" and e.issue is not None:
            by[e.issue].append(e.name)
    return {k: sorted(v) for k, v in by.items() if len(v) > 1}


def coupling_stats(entries: list[TestEntry]) -> dict[str, object]:
    harness = Counter()
    includes = Counter()
    imports = Counter()
    cats = Counter()
    for e in entries:
        if e.location != "issues":
            continue
        for h in e.harness:
            harness[h] += 1
        for i in e.includes:
            includes[i] += 1
        for i in e.imports:
            imports[i] += 1
        cats[e.category] += 1
    return {
        "harness": harness.most_common(20),
        "includes": includes.most_common(15),
        "imports": imports.most_common(15),
        "categories": cats.most_common(),
    }


def render_markdown(entries: list[TestEntry]) -> str:
    today = date.today().isoformat()
    by_theme: dict[str, list[TestEntry]] = defaultdict(list)
    by_loc = Counter(e.location for e in entries)
    for e in entries:
        by_theme[e.theme].append(e)

    issues_only = [e for e in entries if e.location == "issues"]
    domain_only = [e for e in entries if e.location == "domain"]
    multi = multi_issue_groups(entries)
    coupling = coupling_stats(entries)
    batch_drivers = [e for e in entries if "batch_driver" in e.flags]
    small = [e for e in issues_only if "small" in e.flags]
    phase = [e for e in issues_only if "phase_slice" in e.flags]

    lines: list[str] = []
    a = lines.append

    a("# Legacy test inventory")
    a("")
    a("**Issue:** [#1957](https://github.com/cybrid-systems/aura/issues/1957)")
    a(f"**Generated:** {today} by `scripts/inventory_legacy_tests.py`")
    a("**Status:** living document — re-run the script after consolidations.")
    a("")
    a("## Purpose")
    a("")
    a(
        "Categorize legacy per-issue regression tests so we can migrate them in "
        "batches into the preferred `tests/domain/` structure (and existing "
        "family batch drivers under `tests/test_*_batch.cpp`)."
    )
    a("")
    a("Do **not** add new `tests/issues/test_issue_*.cpp` files.")
    a("")
    a("## Scope snapshot")
    a("")
    a("| Location | Count | Notes |")
    a("|----------|------:|-------|")
    a(f"| `tests/issues/test_issue_*.cpp` | {by_loc.get('issues', 0)} | Legacy per-issue mains / bundle members |")
    a(f"| `tests/test_*.cpp` (issue-oriented) | {by_loc.get('root', 0)} | Numbered root tests + `*_batch` drivers |")
    a(f"| `tests/domain/test_*.cpp` | {by_loc.get('domain', 0)} | Preferred destination suites |")
    a(f"| **Total scanned** | **{len(entries)}** | |")
    a("")
    a("### Related artifacts")
    a("")
    a(
        "- Coarser 5-bucket Phase-2 map: "
        "[`tests/domain_classification.md`](domain_classification.md) "
        "(`scripts/classify_test_issues.py`)"
    )
    a("- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)")
    a("- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)")
    a("- Test layout rules: [`tests/README.md`](README.md)")
    a("")

    # ── Theme distribution ────────────────────────────────────────────────
    a("## Theme buckets (8 + uncategorized)")
    a("")
    a(
        "Classification uses the **filename + first 50 lines** (keywords and "
        "filename token boosts). Ties break toward earlier themes in the "
        "priority order below."
    )
    a("")
    a("| Theme | Title | Issues | Root | Domain | Total | Migration priority |")
    a("|-------|-------|-------:|-----:|-------:|------:|--------------------|")
    for theme in THEME_ORDER + ["uncategorized"]:
        bucket = by_theme.get(theme, [])
        if not bucket and theme == "uncategorized":
            continue
        meta = THEME_META[theme]
        n_i = sum(1 for e in bucket if e.location == "issues")
        n_r = sum(1 for e in bucket if e.location == "root")
        n_d = sum(1 for e in bucket if e.location == "domain")
        a(f"| `{theme}` | {meta['title']} | {n_i} | {n_r} | {n_d} | {len(bucket)} | {meta['priority']} |")
    a("")

    # ── Patterns & coupling ───────────────────────────────────────────────
    a("## Patterns, harness usage, coupling")
    a("")
    a("### Harness / entry-point patterns (`tests/issues/` only)")
    a("")
    a("| Pattern | Count | Meaning |")
    a("|---------|------:|---------|")
    harness_help = {
        "test_harness": '`#include "test_harness.hpp"` + CHECK/TEST macros',
        "issue_test_harness": "Older issue-specific harness helper",
        "AURA_ISSUE_TEST": "Macro registration (preferred for new domain cases)",
        "CompilerService": "Integration path via `CompilerService` / eval",
        "RUN_ALL_TESTS": "Harness runner main",
        "own_main": "File defines `int main()` (standalone or bundle source)",
        "bundle_run_fn": "`aura_issue_*_run()` entry for issue bundles",
    }
    for name, count in coupling["harness"]:  # type: ignore[index]
        a(f"| `{name}` | {count} | {harness_help.get(name, '')} |")
    a("")
    a("### `@category` distribution (issues/)")
    a("")
    for cat, count in coupling["categories"]:  # type: ignore[index]
        a(f"- `{cat}`: {count}")
    a("")
    a("### Top includes (first 50 lines, issues/)")
    a("")
    for inc, count in coupling["includes"]:  # type: ignore[index]
        a(f"- `{inc}` — {count}")
    a("")
    a("### Top module imports (first 50 lines, issues/)")
    a("")
    for imp, count in coupling["imports"]:  # type: ignore[index]
        a(f"- `{imp}` — {count}")
    a("")
    a("### Coupling notes")
    a("")
    a(
        "1. **CompilerService-heavy** (~majority of issues/): most legacy tests "
        "are integration-style closed loops (eval → mutate → query stats). "
        "Domain migration should keep a shared CS fixture, not re-copy setup."
    )
    a(
        "2. **Observability dual-path**: many files named `*_observability.cpp` "
        "or probing `query:*-stats` / `engine:metrics`. Prefer folding into "
        "`tests/domain/cases/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`."
    )
    a(
        "3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): "
        "physical file location still `tests/issues/`; migration must update "
        "`issue_link_profiles.json` / CMake when deleting sources."
    )
    a(
        "4. **Internal headers**: direct includes of "
        "`compiler/observability_metrics.h`, `serve/fiber.h`, "
        "`compiler/aura_jit*.h` couple tests to private surfaces — domain "
        "suites should prefer public query/primitives where possible."
    )
    a(
        "5. **Existing consolidation path**: family `*_batch.cpp` drivers under "
        "`tests/` (listed in `AuraDomainTests.cmake`) are the intermediate "
        "step; domain suites are the long-term home."
    )
    a("")

    # ── Duplicate / multi-file / low-value signals ────────────────────────
    a("## Multi-file issues, phase slices, low-value signals")
    a("")
    a(f"- Issue numbers with **multiple** `tests/issues/` files: **{len(multi)}**")
    a(f"- Phase-slice files (`*_phase*`): **{len(phase)}**")
    a(f"- Small files (< 4 KiB, possible thin probes): **{len(small)}**")
    a(f"- Existing `*_batch` drivers (migration milestones): **{len(batch_drivers)}**")
    a("")
    a("### Multi-file issue groups (consolidate first)")
    a("")
    for num, names in sorted(multi.items(), key=lambda kv: (-len(kv[1]), kv[0]))[:25]:
        a(f"- **#{num}** ({len(names)}): " + ", ".join(f"`{n}`" for n in names))
    if len(multi) > 25:
        a(f"- … and {len(multi) - 25} more multi-file issue numbers")
    a("")
    a("### Smallest issue tests (triage for obs-matrix fold or drop)")
    a("")
    for e in sorted(issues_only, key=lambda x: x.size_bytes)[:20]:
        a(f"- `{e.name}` ({e.size_bytes} B) → `{e.theme}` — {e.summary[:80]}")
    a("")
    a("### Batch drivers already present")
    a("")
    for e in sorted(batch_drivers, key=lambda x: x.name):
        a(f"- `{e.rel}` → theme `{e.theme}`")
    a("")
    a("### Domain suites (do not regress; extend these)")
    a("")
    for e in sorted(domain_only, key=lambda x: x.name):
        a(f"- `{e.rel}`")
    a("")

    # ── Migration roadmap ─────────────────────────────────────────────────
    a("## Migration priority roadmap")
    a("")
    a(
        "Suggested order starts with well-contained groups (per #1957) and "
        "leverages existing batch/domain footholds. Each wave should:"
    )
    a("")
    a("1. Pick a theme slice (or multi-file issue group).")
    a("2. Port ACs into a domain suite or family batch driver.")
    a("3. Delete or EXCLUDE the old `test_issue_*.cpp` + update bundles/CMake.")
    a("4. Re-run this inventory script; commit the refreshed markdown.")
    a("")
    a("| Wave | Theme / slice | Why first | Suggested follow-up issue |")
    a("|-----:|---------------|-----------|---------------------------|")
    a(
        "| 1 | `arena_compaction` + compact/gc batches | Contained core; "
        "`test_compact_batch` / `test_compact_sweep_batch` / `test_gc_batch` exist | "
        "Open: *Migrate arena/compaction issue tests → domain* |"
    )
    a(
        "| 2 | Multi-file phase groups (#436, #435, #501, #411) | "
        "Obvious consolidate wins (same issue, many mains) | "
        "Open: *Collapse phase/followup issue test clusters* |"
    )
    a(
        "| 3 | `mutation_dirty` thin obs probes | Largest issues/ bucket; "
        "domain typed-mutate + mutation_boundary batch | "
        "Open: *Mutation/dirty issue tests → domain* |"
    )
    a(
        "| 4 | `fiber_orch` remaining gates | Domain fiber orchestration suite "
        "already swallowed #810/#812/#813/#875-style checks | "
        "Open: *Finish fiber/orch obs migration* |"
    )
    a(
        "| 5 | `linear_ownership` + `shape_soa` | Small counts; batch drivers exist | "
        "Open: *Linear + SoA batch → domain* |"
    )
    a("| 6 | `edsl_hygiene` | Domain hygiene suite + macro_reflect batch | Open: *Hygiene/EDSL issue tests → domain* |")
    a(
        "| 7 | `observability` schema-only files | Fold into "
        "`obs_schema_cases.hpp` matrix | "
        "Open: *Obs schema matrix completion* |"
    )
    a(
        "| 8 | `jit_incremental` smoke ACs | Keep heavy JIT stress in bundles; "
        "move light AC gates only | "
        "Open: *JIT/incremental AC smoke → domain* |"
    )
    a(
        "| 9 | `uncategorized` + early_issue (<#200) | Manual triage; "
        "some may be obsolete vs suite/regression | "
        "Open: *Legacy early-issue triage* |"
    )
    a("")
    a("### Acceptance checkpoints per wave")
    a("")
    a("- No new `test_issue_*.cpp` introduced.")
    a("- Domain or batch binary covers former ACs (or intentional drop documented).")
    a("- `python3 scripts/inventory_legacy_tests.py --check` stays green after refresh.")
    a("- Bundle profiles / CMake targets updated when sources removed.")
    a("")

    # ── Per-theme file lists ──────────────────────────────────────────────
    a("## Per-theme file lists")
    a("")
    a("Files listed as ``location/name`` with issue id and one-line summary.")
    a("")
    for theme in THEME_ORDER + ["uncategorized"]:
        bucket = sorted(by_theme.get(theme, []), key=lambda e: (e.location, e.name))
        if not bucket:
            continue
        meta = THEME_META[theme]
        a(f"### `{theme}` — {meta['title']} ({len(bucket)})")
        a("")
        a(f"**Target:** {meta['target']}")
        a("")
        a(f"**Priority:** {meta['priority']}")
        a("")
        # Split by location for readability
        for loc in ("domain", "root", "issues"):
            sub = [e for e in bucket if e.location == loc]
            if not sub:
                continue
            a(f"#### {loc}/ ({len(sub)})")
            a("")
            for e in sub:
                issue_s = f"#{e.issue}" if e.issue is not None else "—"
                flag_s = f" [{', '.join(e.flags)}]" if e.flags else ""
                summary = e.summary or e.reason or ""
                summary = summary.replace("|", "\\|")
                a(f"- `{e.rel}` ({issue_s}){flag_s} — {summary[:100]}")
            a("")

    # ── Regenerating ──────────────────────────────────────────────────────
    a("## Regenerating")
    a("")
    a("```bash")
    a("python3 scripts/inventory_legacy_tests.py")
    a("python3 scripts/inventory_legacy_tests.py --check")
    a("```")
    a("")
    a(
        "The coarser Phase-2 5-domain classifier remains available as "
        "`scripts/classify_test_issues.py` for historical comparison; **this "
        "inventory (#1957) is the planning source of truth** for domain migration."
    )
    a("")

    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        default=DEFAULT_OUT,
        help=f"markdown output (default: {DEFAULT_OUT})",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if output file is missing or differs from regeneration",
    )
    ap.add_argument(
        "--json",
        type=Path,
        default=None,
        help="optional JSON dump of entries",
    )
    args = ap.parse_args()

    entries = collect()
    if not entries:
        print("error: no test files found", file=sys.stderr)
        return 1

    md = render_markdown(entries)
    # Ensure trailing newline
    if not md.endswith("\n"):
        md += "\n"

    by_theme = Counter(e.theme for e in entries)
    by_loc = Counter(e.location for e in entries)
    print(f"Scanned {len(entries)} files: {dict(by_loc)}")
    print("Theme distribution:")
    for t in THEME_ORDER + ["uncategorized"]:
        if by_theme.get(t):
            print(f"  {t:22s} {by_theme[t]:4d}")

    if args.json:
        payload = {
            "generated": date.today().isoformat(),
            "count": len(entries),
            "entries": [asdict(e) for e in entries],
        }
        args.json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.json}")

    out: Path = args.output
    if args.check:
        if not out.is_file():
            print(f"CHECK FAIL: missing {out}", file=sys.stderr)
            return 1
        existing = out.read_text(encoding="utf-8")
        if existing != md:
            print(f"CHECK FAIL: {out} is stale — re-run without --check", file=sys.stderr)
            return 1
        print(f"CHECK OK: {out} up to date")
        return 0

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(md, encoding="utf-8")
    print(f"wrote {out} ({len(md)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
