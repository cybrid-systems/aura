#!/usr/bin/env python3
"""Phase 2 migration: classify 635 test_issue_*.cpp into 5 domain files.

Anqi 05:43 directive (Phase 2 of tests/ consolidation):
  Generate migration report + create 5 domain manifest files.

Dry-run by default. Pass --migrate to actually write the domain files.

Domain assignment (based on @reason text + first-100-lines keyword scan):
  fiber        — src/serve/fiber* / GC safepoint / steal / resume
  ir           — src/compiler/{evaluator,lowering,pass_manager,optimization_passes}* + parser* + ir*
  observability — src/compiler/observability* / stats* / metrics* / counters*
  mutation     — src/core/mutation* / dirty_propagation* / post_invalidate* / bump_steal*
  persist      — src/serve/persist* / src/stdlib* / save* / load*

Default domain (unmatched): ir (largest catch-all).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

# Use env var AURA_ROOT (set by caller) or auto-detect from /tmp/ placement.
AURA_ROOT = Path(os.environ.get("AURA_ROOT", str(Path(__file__).resolve().parents[1])))
ISSUES_DIR = AURA_ROOT / "tests" / "issues"
TESTS_DIR = AURA_ROOT / "tests"

DOMAINS = ["fiber", "ir", "observability", "mutation", "persist"]

DOMAIN_KEYWORDS: dict[str, list[str]] = {
    "fiber": [
        "fiber",
        "GC safepoint",
        "gc_safepoint",
        "steal",
        "resume",
        "yield",
        "fiber_mutation",
        "fiber_join",
        "fiber_macro",
    ],
    "ir": [
        "IR",
        "ir_",
        "ir_cache",
        "ir_soa",
        "ir_closure",
        "ir_executor",
        "ir_hygiene",
        "ir_metadata",
        "lowering",
        "parser",
        "evaluator",
        "optimization_passes",
        "pass_manager",
        "soa_view",
        "dirty_propagation",
        "compute_kind",
        "constant_folding",
        "sv_ir",
        "adt_runtime",
        "value_impl",
        "ir_metadata_interpreter",
        "relower",
        "linear_types",
        "type_checker",
        "typechecker",
    ],
    "observability": [
        "observability",
        "stats",
        "metrics",
        "counter",
        "snapshot",
        "engine:metrics",
        "stats_",
        "metrics_",
        "observability_metrics",
        "compiler_metrics",
        "primitive_surface",
    ],
    "mutation": [
        "mutation",
        "mutate",
        "post_invalidate",
        "bump_steal",
        "post_mutate",
        "post_steal",
        "pre_steal",
        "occ_cache",
        "invariant",
        "bump",
        "panic_checkpoint",
    ],
    "persist": [
        "persist",
        "save",
        "load",
        "stdlib",
        "recover",
    ],
}


def classify_file(path: Path) -> str:
    """Return the best-fit domain for a test_issue_NNN.cpp."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "ir"
    head = text[:4000]
    score: dict[str, int] = {d: 0 for d in DOMAINS}
    for domain, keywords in DOMAIN_KEYWORDS.items():
        for kw in keywords:
            score[domain] += head.count(kw)
    best = max(score, key=score.get)
    if score[best] == 0:
        return "ir"
    return best


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--migrate", action="store_true", help="actually write the 5 domain files (default: dry-run report only)"
    )
    ap.add_argument(
        "--report",
        type=Path,
        default=TESTS_DIR / "domain_classification.md",
        help="migration report output path (default: tests/domain_classification.md)",
    )
    args = ap.parse_args()

    issue_files = sorted(ISSUES_DIR.glob("test_issue_*.cpp"))
    print(f"Found {len(issue_files)} test_issue files in {ISSUES_DIR}")
    if not issue_files:
        print("no test_issue files found")
        return 1

    by_domain: dict[str, list[Path]] = {d: [] for d in DOMAINS}
    for p in issue_files:
        d = classify_file(p)
        by_domain.setdefault(d, []).append(p)

    total = sum(len(v) for v in by_domain.values())
    print(f"\nClassification (total {total} files):")
    for d in DOMAINS:
        files = by_domain[d]
        print(f"  {d:14s}: {len(files):3d} files")

    # Generate migration report (markdown)
    report_lines = [
        "# Phase 2 migration classification (tests/issues/ → 5 domain files)",
        "",
        "**Generated:** auto by `scripts/classify_test_issues.py`",
        f"**Total test_issue files:** {total}",
        "",
        "## Domain distribution",
        "",
        "| Domain | Count |",
        "|--------|------:|",
    ]
    for d in DOMAINS:
        report_lines.append(f"| {d} | {len(by_domain[d])} |")
    report_lines.extend(["", "## Per-domain file lists", ""])
    for d in DOMAINS:
        report_lines.append(f"### {d} ({len(by_domain[d])} files)")
        report_lines.append("")
        for p in by_domain[d]:
            report_lines.append(f"- `{p.name}`")
        report_lines.append("")

    args.report.write_text("\n".join(report_lines))
    print(f"\n  wrote migration report: {args.report}")

    if not args.migrate:
        print("\nDRY RUN — pass --migrate to also write tests/test_<domain>.cpp manifest files")
        return 0

    # Actual migration: write 5 domain manifest files
    for d in DOMAINS:
        files = by_domain[d]
        domain_file = TESTS_DIR / f"test_{d}.cpp"
        lines = [
            "// Auto-generated by scripts/classify_test_issues.py --migrate",
            f"// Phase 2 of tests/ consolidation (Anqi 05:43 directive): {d} domain.",
            "// Per-issue test bodies still live in tests/issues/test_issue_*.cpp",
            "// (Phase 4+ migration will physically consolidate them here).",
            "//",
            f"// {len(files)} test_issue_NNN.cpp files in this domain.",
            "//",
            "// Phase 3 habit change: new {d} tests should add AURA_ISSUE_TEST(NNN, ...)",
            "// here instead of creating new tests/issues/test_issue_NNN.cpp files.",
            "",
            '#include "test_harness.hpp"',
            "",
            "// (no test bodies yet — {d} tests are in tests/issues/test_issue_*.cpp)",
            "",
            "int main() {",
            "    // placeholder — Phase 4+ will add {d} tests via AURA_ISSUE_TEST macros",
            "    return 0;",
            "}",
            "",
        ]
        domain_file.write_text("\n".join(lines))
        print(f"  wrote {domain_file} ({len(files)} files in domain)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
