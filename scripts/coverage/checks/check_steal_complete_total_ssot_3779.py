#!/usr/bin/env python3
"""Issue #3779: steal_complete_total Agent-visible SSOT (gc_hooks face).

Residual: same C++ / Agent-facing name on three atomics; only gc_hooks +
AdaptiveStealStats always bumped at aura_evaluator_on_steal_complete.
CompilerMetrics::steal_complete_total lived on fields.inc / :group dumps
but was only conditionally mirrored mid-body (or missed when scheduler
hooks evaluator was null). query hashes already read gc_hooks — Agents
comparing (engine:metrics :group) vs query hashes could disagree.

Contract (one row per AC):
  AC1  on_steal_complete entry always bumps gc_hooks + AdaptiveStealStats
       + CompilerMetrics.steal_complete_total (when metrics live) — one
       CompilerMetrics bump site in the entry window (no mid-body re-bump)
  AC2  dump_metric_groups overlays steal_complete_total from
       gc_hooks::steal_complete_total() (process SSOT for :group/:prefix)
  AC3  Existing orch/query keys unchanged (no schema-3779; no rename)
  AC4  Soft/Off: no production gate on the metric bumps / overlay
  AC5  No mid-struct insert; no test_issue_3779.cpp; no docs/design/;
       linter after #3737 compiler_metrics_fields ABI check

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    obm = _read("src/compiler/observability_metrics.h")
    inc = _read("src/compiler/compiler_metrics_fields.inc")
    metrics = _read("src/serve/metrics.h")
    hooks = _read("src/core/gc_hooks.h")
    build = _read("build.py")
    test_facade = _read("tests/compiler/test_engine_metrics_facade.cpp")
    test_steal = _read("tests/serve/test_steal_complete_gc_defer.cpp")

    fn = efm.find('extern "C" void aura_evaluator_on_steal_complete')
    if fn < 0:
        fails.append("AC1: on_steal_complete missing")
        fn_win = ""
    else:
        # Entry window through LayoutStamp dual-check (#2351) or ~4k chars
        end = efm.find("Issue #2351", fn)
        if end < 0:
            end = fn + 4500
        fn_win = efm[fn:end]

    must("Issue #3779", "AC1 cite", efm)
    must("g_steal_complete_total.fetch_add", "AC1 gc_hooks bump", fn_win)
    must("adaptive_steal_stats().steal_complete_total.fetch_add", "AC1 AdaptiveStealStats", fn_win)
    must("m->steal_complete_total.fetch_add", "AC1 CompilerMetrics entry bump", fn_win)
    must("evaluator_for_scheduler_hooks()", "AC1 hooks evaluator", fn_win)

    # Exactly one CompilerMetrics steal_complete bump in the function entry
    # window (before LayoutStamp section) — no mid-body re-bump.
    bumps = len(re.findall(r"m->steal_complete_total\.fetch_add", fn_win))
    if bumps != 1:
        fails.append(f"AC1: expected 1 CompilerMetrics steal_complete bump in entry, got {bumps}")

    # Mid-body residual after LayoutStamp must not re-bump steal_complete_total
    rest = efm[end:] if fn >= 0 and end > 0 else ""
    # Limit to end of this function roughly (next strong ABI marker)
    next_fn = rest.find('extern "C"')
    rest_fn = rest[:next_fn] if next_fn > 0 else rest[:8000]
    if "m->steal_complete_total.fetch_add" in rest_fn:
        fails.append("AC1: mid-body CompilerMetrics steal_complete re-bump present")

    must("Issue #3779", "AC2 dump cite", obs)
    must("steal_complete_total", "AC2 overlay field", obs)
    must("gc_hooks::steal_complete_total()", "AC2 overlay SSOT", obs)
    must('groups.find("telemetry")', "AC2 telemetry group", obs)

    must("AURA_COMPILER_METRICS_FIELD(steal_complete_total)", "AC3 .inc retained", inc)
    must("steal_complete_total{0}; // #2203", "AC3 AdaptiveStealStats key", metrics)
    must("g_steal_complete_total{0}", "AC3 gc_hooks key", hooks)
    must_not("schema-3779", "AC3 no new query schema", obs)
    must_not('"steal-complete-total-ssot"', "AC3 no renamed query key", obs)

    # Soft/Off: metric path must not be gated on production_defaults_active
    # for the entry SSOT bump block.
    if "production_defaults_active()" in fn_win and "steal_complete_total" in fn_win:
        # Allow cite elsewhere in window; forbid gating the bump itself.
        bump_idx = fn_win.find("m->steal_complete_total.fetch_add")
        gate_near = fn_win[max(0, bump_idx - 200) : bump_idx]
        if "production_defaults_active()" in gate_near:
            fails.append("AC4: CompilerMetrics steal_complete bump gated on production")

    must("check_compiler_metrics_fields_3737", "AC5 prev ABI linter", build)
    must("check_steal_complete_total_ssot_3779", "AC5 build.py", build)
    prev = build.find("check_compiler_metrics_fields_3737")
    ours = build.find("check_steal_complete_total_ssot_3779")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3737")

    must("#3779", "AC5 facade test cite", test_facade)
    must("#3779", "AC5 steal test cite", test_steal)
    must("Issue #3779", "AC5 metrics comment", obm)

    if _read("tests/compiler/test_issue_3779.cpp") or _read("tests/serve/test_issue_3779.cpp"):
        fails.append("AC5: test_issue_3779.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3779-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    # Append-only: steal_complete_total still present; no mid-struct rewrite
    # of compiler_metrics_fields.inc order (name remains after naked_mutate).
    lines = [ln.strip() for ln in inc.splitlines() if ln.startswith("AURA_COMPILER_METRICS_FIELD(")]
    try:
        i_steal = lines.index("AURA_COMPILER_METRICS_FIELD(steal_complete_total)")
        i_naked = lines.index("AURA_COMPILER_METRICS_FIELD(naked_mutate_fail_closed_wired)")
        if i_steal != i_naked + 1:
            fails.append("AC5: steal_complete_total moved mid-.inc (append-only violated)")
    except ValueError:
        fails.append("AC5: steal_complete_total / naked_mutate order parse fail")

    if fails:
        print(f"FAIL #3779 steal_complete_total_ssot ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3779 steal_complete_total_ssot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
