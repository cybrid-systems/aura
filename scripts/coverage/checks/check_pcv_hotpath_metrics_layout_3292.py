#!/usr/bin/env python3
"""Issue #3292: PcvHotpathMetrics must stay append-only at struct END (#2906)
with compile-time layout stamps (I3 machine-checkable residual).

#2906 fixed FlatAST locked mutate exclusive PCV and appended metrics at
struct END of PcvHotpathMetrics. Root cause of the earlier revert was a
mid-struct field insertion: stale module BMIs wrote at wrong offsets and
corrupted neighboring heap (IR cache string keys → double free). The
append-only discipline was comment + human review only. This linter makes
it machine-checkable: static_assert(offsetof(...)) on the last metrics +
struct size, so a mid-struct insert fails the build.

Gate rows:
  G1  persistent_child_vector.hh has static_assert offsetof stamps citing
      Issue #3292 next to PcvHotpathMetrics (#2906 lineage comment).
  G2  the last metric (stale_span_force_exclusive_total) is pinned at
      offset 128 and pcv_span_stale_across_guard_total at 112 —
      inserting any field before them shifts the offsets and fails.
  G3  sizeof(PcvHotpathMetrics) pinned to 136 (append-only growth changes
      the size only when a field is appended, which must be a deliberate
      update of these asserts).
  G4  no new runtime counters / atomics (g_3292_* absent); Soft/Off zero
      extra cost (compile-time only).
  G5  test ACs in tests/compiler/test_pcv_children_safe_default_migration
      (#81967 suite home); no test_issue_3292.cpp; no docs/design/ (#1655).
  G6  build.py wires this linter.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    pcv = _read("src/core/persistent_child_vector.hh")
    test = _read("tests/compiler/test_pcv_children_safe_default_migration.cpp")
    build = _read("build.py")

    # ── G1: static_assert stamps next to the struct (#2906 lineage) ──
    must("Issue #3292" in pcv, "G1: persistent_child_vector.hh cites Issue #3292")
    must("#2906" in pcv, "G1: #2906 lineage comment present")
    must("append-only at struct END" in pcv, "G1: append-only discipline documented")
    struct_pos = pcv.find("struct PcvHotpathMetrics")
    must(struct_pos >= 0, "G1: PcvHotpathMetrics struct present")
    if struct_pos >= 0:
        # Asserts must sit immediately after the struct (same namespace
        # block) so they guard the definition, not a distant copy.
        tail = pcv[struct_pos : struct_pos + 3200]
        must(
            "static_assert(offsetof(PcvHotpathMetrics, stale_span_force_exclusive_total)" in tail,
            "G1: last-metric offsetof assert next to the struct",
        )

    # ── G2: pinned offsets (mid-struct insert fails) ──
    must(
        "offsetof(PcvHotpathMetrics, stale_span_force_exclusive_total) == 128" in pcv,
        "G2: stale_span_force_exclusive_total pinned at offset 128",
    )
    must(
        "offsetof(PcvHotpathMetrics, pcv_span_stale_across_guard_total) == 112" in pcv,
        "G2: pcv_span_stale_across_guard_total pinned at offset 112",
    )
    must(
        "offsetof(PcvHotpathMetrics, flatast_locked_move_out_exclusive_total) == 96" in pcv,
        "G2: flatast_locked_move_out_exclusive_total pinned at offset 96",
    )

    # ── G3: sizeof pinned ──
    must("sizeof(PcvHotpathMetrics) == 136" in pcv, "G3: sizeof pinned to 136")

    # ── G4: no new runtime counters (source only; test files may
    #    legitimately reference the string in CHECK assertions) ──
    must("g_3292_" not in pcv, "G4: no new g_3292_* runtime counter in source")

    # ── G5: src-aligned suite home (#81967) ──
    must("ac3292_1_layout_stamps_present" in test, "G5: AC1 test present")
    must("ac3292_2_offsets_pinned" in test, "G5: AC2 test present")
    must("ac3292_3_no_new_runtime" in test, "G5: AC3 test present")
    must(not _read("tests/compiler/test_issue_3292.cpp"), "G5: no tests/compiler/test_issue_3292.cpp per #81967")
    must(not _read("tests/issues/test_issue_3292.cpp"), "G5: no tests/issues/test_issue_3292.cpp per #81967")

    # ── G6: build.py wiring ──
    must("check_pcv_hotpath_metrics_layout_3292" in build, "G6: build.py wires linter")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        bad = [f.name for f in sorted(docs.glob("3292-*"))]
        must(not bad, "G6: no docs/design/3292-* per #1655")
    else:
        must(True, "G6: no docs/design/3292-* per #1655")

    if failures:
        print(f"\n#3292 linter: {len(failures)} gate(s) FAILED")
        return 1
    print("\n#3292 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
