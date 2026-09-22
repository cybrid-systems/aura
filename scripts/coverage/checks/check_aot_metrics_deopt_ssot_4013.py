#!/usr/bin/env python3
"""Issue #4013: aot_metrics weak stubs forward live deopt SSOT.

jit_closure_stale_deopt_total / jit_closure_safe_fallbacks were dead under
fork-isolated / light-link order: weak stubs in aura_jit_bridge_stub.cpp
returned 0 and won ELF first-def over (or instead of) production bridge.
Live signal is aura_deopt_inc → g_workspace_deopt_count (aura_deopt_count).

Contract (one row per AC):
  AC1  weak stubs forward aura_deopt_count() (not hard-coded 0)
  AC2  production strong readers still defined in aura_jit_bridge.cpp
  AC3  aura_deopt_count / aura_deopt_inc / g_workspace_deopt_count SSOT live
  AC4  query:jit-deopt-stats additive (register_stats_impl); old query keys
       unchanged; no mid-struct metrics insert
  AC5  optional nm/objdump gate when a light/fork binary is present — symbols
       resolve (W/T) and stub object references aura_deopt_count; skip cleanly
       when no binary (box may lack GCC 16)
  AC6  Soft/Off observe-only; no test_issue_4013.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _extract_fn(hay: str, name: str, window: int = 400) -> str:
    # Match weak or strong C ABI definition near name.
    m = re.search(
        rf'extern\s+"C"[^{{|;]*\b{re.escape(name)}\s*\([^)]*\)\s*(?:noexcept\s*)?\{{',
        hay,
    )
    if not m:
        return ""
    return hay[m.start() : m.start() + window]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    runtime = _read("src/compiler/aura_jit_runtime.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    metrics = _read("src/compiler/observability_metrics.h")

    # AC1 — weak stubs forward live SSOT
    stale_stub = _extract_fn(stub, "aura_jit_closure_stale_deopt_total")
    safe_stub = _extract_fn(stub, "aura_jit_closure_safe_fallbacks")
    must("aura_jit_closure_stale_deopt_total", "AC1 stub symbol", stub)
    must("aura_jit_closure_safe_fallbacks", "AC1 stub symbol", stub)
    must("__attribute__((weak))", "AC1 stale weak", stale_stub)
    must("__attribute__((weak))", "AC1 safe weak", safe_stub)
    must("aura_deopt_count()", "AC1 stale forwards SSOT", stale_stub)
    must("aura_deopt_count()", "AC1 safe forwards SSOT", safe_stub)
    must("Issue #4013", "AC1 cite", stub)
    # Dead hard-zero body must not remain in the reader stubs.
    if re.search(
        r"aura_jit_closure_stale_deopt_total\s*\([^)]*\)[^{]*\{\s*return 0\s*;",
        stub,
    ):
        fails.append("AC1: stale_deopt_total still hard-returns 0")
    if re.search(
        r"aura_jit_closure_safe_fallbacks\s*\([^)]*\)[^{]*\{\s*return 0\s*;",
        stub,
    ):
        fails.append("AC1: safe_fallbacks still hard-returns 0")

    # AC2 — production strong readers remain
    must("aura_jit_closure_stale_deopt_total", "AC2 bridge", bridge)
    must("aura_jit_closure_safe_fallbacks", "AC2 bridge", bridge)
    must("jit_closure_stale_deopt_total", "AC2 production bump", bridge)
    must("jit_closure_safe_fallbacks", "AC2 production bump", bridge)

    # AC3 — live SSOT
    must("g_workspace_deopt_count", "AC3 SSOT atomic", runtime)
    must("aura_deopt_inc", "AC3 writer", runtime)
    must("aura_deopt_count", "AC3 reader", runtime)

    # AC4 — additive query; no old-key rewrite; no mid-struct insert
    must("query:jit-deopt-stats", "AC4 new query", obs)
    must("register_stats_impl", "AC4 SlimSurface-safe", obs)
    must("aura_deopt_count()", "AC4 query reads SSOT", obs)
    must("schema", "AC4 schema key", obs)
    # Old keys still present (not renamed/removed).
    must('insert_kv("jit_closure_stale_deopt_total"', "AC4 old key kept", obs)
    must('insert_kv("jit_closure_safe_fallbacks"', "AC4 old key kept", obs)
    # Metrics struct still has the fields (no mid-insert invent); cite end-ish.
    must("jit_closure_stale_deopt_total", "AC4 metrics field", metrics)
    must("jit_closure_safe_fallbacks", "AC4 metrics field", metrics)
    must_not("test_issue_4013", "AC6 no invent test", obs)

    # AC5 — optional binary nm/objdump gate
    candidates = [
        ROOT / "build" / "test_jit_metrics",
        ROOT / "build" / "tests" / "test_jit_metrics",
        ROOT / "build" / "test_spec_jit",
        ROOT / "build-release" / "test_jit_metrics",
    ]
    binary = next((c for c in candidates if c.is_file()), None)
    nm = shutil.which("nm")
    if binary and nm:
        try:
            out = subprocess.check_output(
                [nm, "-C", str(binary)],
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=60,
            )
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            fails.append(f"AC5: nm failed on {binary.name}: {e}")
            out = ""
        if out:
            for sym in (
                "aura_jit_closure_stale_deopt_total",
                "aura_jit_closure_safe_fallbacks",
                "aura_deopt_count",
            ):
                if sym not in out:
                    fails.append(f"AC5: {sym} missing from nm of {binary.name}")
            # Accept weak (W) or text (T) — forward stub or strong bridge.
            for sym in (
                "aura_jit_closure_stale_deopt_total",
                "aura_jit_closure_safe_fallbacks",
            ):
                lines = [ln for ln in out.splitlines() if sym in ln]
                if not lines:
                    continue
                if not any(re.search(r"\s[WTtw]\s", ln) for ln in lines):
                    fails.append(f"AC5: {sym} not W/T in {binary.name}: {lines[:2]}")
    else:
        # Source-level stand-in when no binary / no nm (land without GCC 16).
        must("return aura_deopt_count()", "AC5 source-forward gate", stub)

    # AC6 — no forbidden invent paths
    if _read("tests/compiler/test_issue_4013.cpp") or _read("tests/issues/test_issue_4013.cpp"):
        fails.append("AC6: test_issue_4013.cpp present (forbidden #81967)")
    if _read("docs/design/4013-aot-metrics-deopt-ssot.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #4013 aot_metrics deopt SSOT:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #4013 aot_metrics deopt SSOT: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
