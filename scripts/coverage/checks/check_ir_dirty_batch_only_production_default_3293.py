#!/usr/bin/env python3
"""Issue #3293: production residual multi-via-single stays hard-abort by
default — machine-checkable residual of #3201 (I3).

#3201 (P0, `55aa2e819`) already wired production_defaults → hard face:
`ir_dirty_batch_only_hard()` env unset → `aura_production_defaults_active_probe`
(strong def in typed_mutation_audit_hooks.cpp returns
`typed_audit::production_defaults_active()`), and the streak==2 residual note
path bumps `g_ir_soa_batch_only_hard_abort_total` then std::abort(). This
linter makes the production-path arms-hard-abort contract machine-checkable
so a future Soft window under agent packs fails CI (AC5), and AC4 adds a
live fixture that injects single-mark residual under production defaults and
asserts the hard face (SIGABRT via fork).

Gate rows:
  G1  ir_dirty_batch_only_hard(): env unset consults the production probe;
      strong probe def in typed_mutation_audit_hooks.cpp returns
      typed_audit::production_defaults_active() (production path arms
      hard-abort by default; #3201 preserved).
  G2  streak==2 residual note path bumps hard_abort_total and std::abort()
      (abort window #3105 preserved; no Soft re-entry).
  G3  Soft / env=0 / unit: residual stays metric-only (cascades_total +
      marks_total bump path exists; no g_3293_* counter invented).
  G4  all three batch APIs (mark_blocks_dirty / _bits_only /
      mark_all_blocks_dirty) clear residual (clear_single_mark_residual).
  G5  test ACs in test_batch_dirty_discipline (#81967): ac3293_1 live
      SIGABRT fixture, ac3293_2 Soft metric-only, ac3293_3 source/linter.
  G6  no test_issue_3293.cpp; no docs/design/3293-* (#1655/#81967).
  G7  build.py wires this linter.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def main() -> int:
    soa = _read("src/compiler/ir_soa.ixx")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    t = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")

    # ── G1: production path arms hard-abort (env unset → probe) ──
    must("Issue #3293" in soa or "3293" in soa, "G1: ir_soa.ixx cites #3293")
    must("ir_dirty_batch_only_hard" in soa, "G1: hard probe helper present")
    probe = re.search(
        r"inline bool ir_dirty_batch_only_hard\(\)[^{]*\{(?P<body>.*?)\n\}",
        soa,
        re.DOTALL,
    )
    if not probe:
        must(False, "G1: could not extract ir_dirty_batch_only_hard body")
    else:
        body = probe.group("body")
        must("e[0] == '0'" in body, "G1: env=0 force-off retained (Soft escape)")
        must(
            "aura_production_defaults_active_probe" in body,
            "G1: env unset consults production probe (#3201 default-on)",
        )
    must(
        "aura_production_defaults_active_probe" in hooks and "typed_audit::production_defaults_active()" in hooks,
        "G1: strong probe def returns production_defaults_active()",
    )

    # ── G2: streak==2 residual note path → hard_abort bump + abort ──
    note = re.search(
        r"inline void note_single_mark_for_residual\([^{]*\{(?P<body>.*?)\n\}",
        soa,
        re.DOTALL,
    )
    if not note:
        must(False, "G2: could not extract note_single_mark_for_residual")
    else:
        body = note.group("body")
        must("g_ir_soa_batch_only_hard_abort_total.fetch_add" in body, "G2: hard-abort counter bumped on residual")
        must("std::abort()" in body, "G2: abort path present (never green schema-1)")
        must("ir_dirty_batch_only_hard()" in body, "G2: production hard face consulted")

    # ── G3: Soft metric-only preserved; no invented counters ──
    must(
        "g_ir_soa_residual_multi_via_single_cascades_total.fetch_add" in soa,
        "G3: Soft residual cascade metric preserved",
    )
    must("g_ir_soa_residual_multi_via_single_marks_total.fetch_add" in soa, "G3: Soft residual marks metric preserved")
    must("g_3293_" not in soa, "G3: no g_3293_* runtime counter")

    # ── G4: batch APIs clear residual ──
    for api in ("mark_blocks_dirty", "mark_blocks_dirty_bits_only", "mark_all_blocks_dirty"):
        must(
            f"{api}(" in soa and soa.count("clear_single_mark_residual(this)") >= 3,
            f"G4: {api} + batch residual clear present",
        )

    # ── G5: test ACs in src-aligned suite (#81967) ──
    must("ac3293_1_production_hard_face" in t, "G5: AC1 SIGABRT fixture present")
    must("ac3293_2_soft_metric_only" in t, "G5: AC2 Soft metric-only present")
    must("ac3293_3_source_and_linter" in t, "G5: AC3 source/linter present")

    # ── G6: no invent files / docs ──
    must(not _read("tests/compiler/test_issue_3293.cpp"), "G6: no test_issue_3293.cpp (#81967)")
    must(not _read("tests/issues/test_issue_3293.cpp"), "G6: no tests/issues/ (#81967)")

    # ── G7: build.py wiring ──
    must("check_ir_dirty_batch_only_production_default_3293" in build, "G7: build.py wires linter")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        bad = [f.name for f in sorted(docs.glob("3293-*"))]
        must(not bad, "G7: no docs/design/3293-* (#1655)")
    else:
        must(True, "G7: no docs/design/3293-* (#1655)")

    if failures:
        print(f"\n#3293 linter: {len(failures)} gate(s) FAILED")
        return 1
    print("\n#3293 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
