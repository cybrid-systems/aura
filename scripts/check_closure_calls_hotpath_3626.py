#!/usr/bin/env python3
# scripts/check_closure_calls_hotpath_3626.py -- Issue #3626 source-cite gate.
#
# AC1: the apply_closure entry RMW (closure_calls_total) sits behind the
#      production-pack compile-out guard
#      (#if !defined(NDEBUG) || !defined(AURA_PRODUCTION_PACK)) — Soft/unit
#      keep the #252 dual-path counter, production pack + NDEBUG compiles
#      it out (AURA_HOT_RECORD remains the sampled observe arm). Closes
#      the I1 eval tax: process-wide RMW on every apply rode inter-core
#      coherency on the shared entry (#252/#3421 residual).
# AC2: #3421 densify refuse path untouched — refuse is cold and keeps
#      noting stale faces (production_apply_closure_densify_hard_refuse +
#      note_apply_closure_densify_hard_refuse + closure_stale_returns).
# AC3: FFI arm untouched — closure_ffi_calls stays ungated (the FFI refuse
#      invariant is open #3602's territory; this ticket must not touch it).
# AC4: no new metric name / no header change (observability_metrics.h has
#      no #3626 cite), runtime ACs live in
#      tests/compiler/test_obs_metrics_smoke_batch.cpp (run_3626_*); no
#      docs/design/*3626*, no tests/**/test_issue_3626.cpp; linter
#      registered in build.py.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SRC = "src/compiler/evaluator_eval_flat.cpp"
TEST = "tests/compiler/test_obs_metrics_smoke_batch.cpp"
BUILD = "build.py"
METRICS = "src/compiler/observability_metrics.h"

LINTER = "check_closure_calls_hotpath_3626"
ANCHOR = "AURA_HOT_RECORD();"
GUARD = "#if !defined(NDEBUG) || !defined(AURA_PRODUCTION_PACK)"
RMW = "m->closure_calls_total.fetch_add(1, std::memory_order_relaxed);"
CITE = "Issue #3626"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(src: str, test: str, build: str, metrics_hdr: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — entry RMW behind the compile-out guard.
    pos = src.find(ANCHOR)
    if pos == -1:
        fails.append("AC1: AURA_HOT_RECORD anchor not found")
        win = ""
    else:
        win = src[pos : pos + 1500]
        must(GUARD, "AC1 production-pack compile-out guard", win)
        must(CITE, "AC1 #3626 cite in guard window", win)
        must(RMW, "AC1 entry RMW retained inside the guard", win)
        must("#endif", "AC1 guard closed", win)
        if win:
            i_guard = win.find(GUARD)
            i_rmw = win.find(RMW)
            i_end = win.find("#endif")
            if i_rmw != -1 and i_guard != -1 and i_rmw < i_guard:
                fails.append("AC1: RMW must sit inside the guard")
            if i_end != -1 and i_rmw != -1 and i_end < i_rmw:
                fails.append("AC1: #endif closes before the RMW")

    # AC2 — #3421 refuse path untouched.
    must("production_apply_closure_densify_hard_refuse", "AC2 refuse gate present", src)
    must("note_apply_closure_densify_hard_refuse", "AC2 refuse note helper present", src)
    must("closure_stale_returns", "AC2 stale faces note retained", src)

    # AC3 — FFI arm untouched (#3602 territory).
    must("closure_ffi_calls.fetch_add", "AC3 FFI arm counter present ungated", src)

    # AC4 — runtime ACs + registration + no docs / invented test / header change.
    for fn in (
        "run_3626_metrics_smoke",
        "ac3626_1_soft_counter_still_bumps",
    ):
        must(fn, "AC4 runtime AC", test)
    must("check_closure_calls_hotpath_3626", "AC4 build.py registration", build)
    must_not(CITE, "AC4 no new metric / no header change", metrics_hdr)
    design = ROOT / "docs" / "design"
    if design.is_dir():
        hits = sorted(p.name for p in design.glob("*3626*"))
        if hits:
            fails.append(f"AC4: docs/design/*3626* present: {hits}")
    invented = sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("tests/**/test_issue_3626.cpp"))
    if invented:
        fails.append(f"AC4: invented test_issue_3626.cpp present: {invented}")
    return fails


def _self_test() -> int:
    src_ok = (
        "    AURA_HOT_RECORD();\n"
        "    types::note_value_tag_hot_path();\n"
        "    soa_view::record_edsl_apply_soa_path();\n"
        "    // Issue #3626 (#252/#3421 residual): metrics-only under production pack.\n"
        "#if !defined(NDEBUG) || !defined(AURA_PRODUCTION_PACK)\n"
        "    if (compiler_metrics_) {\n"
        "        auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);\n"
        "        m->closure_calls_total.fetch_add(1, std::memory_order_relaxed);\n"
        "    }\n"
        "#endif\n"
        "    production_apply_closure_densify_hard_refuse(arena_, cl_copy);\n"
        "    note_apply_closure_densify_hard_refuse(metrics, *this);\n"
        "    metrics->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);\n"
        "    m->closure_ffi_calls.fetch_add(1, std::memory_order_relaxed);\n"
    )
    test_ok = 'static int run_3626_metrics_smoke() {\n    CHECK(m->closure_calls_total.load(std::memory_order_relaxed) > total0,\n          "ac3626_1_soft_counter_still_bumps: closure_calls_total increments on apply");\n    return g_failed != 0 ? 1 : 0;\n}\n'
    build_ok = 'ROOT / "scripts" / "check_closure_calls_hotpath_3626.py"'
    metrics_ok = "struct CompilerMetrics {\n    std::atomic<std::uint64_t> closure_calls_total{0};\n};"

    good = _rows(src_ok, test_ok, build_ok, metrics_ok)
    if good:
        print(f"self-test: synthetic-good fixture unexpectedly failed: {good}")
        return 1
    stripped = src_ok.replace("#if !defined(NDEBUG) || !defined(AURA_PRODUCTION_PACK)\n", "").replace("#endif\n", "", 1)
    bad = _rows(stripped, test_ok, build_ok, metrics_ok)
    if not any("compile-out guard" in r for r in bad):
        print("self-test: stripped fixture did not trip the AC1 guard row")
        return 1
    print("ok check_closure_calls_hotpath_3626 self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3626 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(SRC), _read(TEST), _read(BUILD), _read(METRICS))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
