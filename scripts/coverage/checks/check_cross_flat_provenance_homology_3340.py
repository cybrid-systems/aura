#!/usr/bin/env python3
"""Issue #3340 linter — cross-FlatAST provenance table-index homology.

Residual of #3278: `clone_macro_body_at_depth` stamps origin via
`source.provenance(body_id)` else weak-link `body_id` (raw uint32).
Provenance is a per-FlatAST MarkerProvenanceTable index (0 = none).
Cross-pool that leftover is orphan/wrong in the TARGET table —
densify/steal can recycle slots. #3278 already zeros non-homologous
schema_cache ids under production / force-hygienic; provenance was
left as a residual.

Fix (minimal, no second hygiene model): same post-clone hook
(`ensure_cross_flat_expand_consistency`), same production / cross-pool
gate, same walk: if `target.provenance(cur) != 0` then
`target.set_provenance(cur, 0)`. Prefer zero (force re-stamp / no
provenance) over inventing a table transplant. Same-pool / Soft/Off:
no walk, provenance copy preserved (zero-cost). No new metric, no new
query key.

Gate rows:
  G1  src/compiler/macro_expansion.cpp cites Issue #3340 in
      ensure_cross_flat_expand_consistency (the cross-flat hook).
  G2  same cross-pool gate as #3278 (target_pool != source_pool).
  G3  production / force-hygienic gate (production_defaults_active or
      g_macro_expand_sandbox_strict) — Soft/Off zero-cost.
  G4  zeros leftover provenance: set_provenance(cur, 0).
  G5  reuses existing g_hygiene_violation_in_macro_expand_total
      (no new metric / no g_3340_*).
  G6  clone_macro_body_at_depth origin stamp still present
      (source-cite for the path that feeds the homology check).
  G7  test ACs in tests/compiler/test_macro_cross_flat_hygiene.cpp
      (the #2235 suite home — issue AC5: extend existing suite).
  G8  build.py wires this linter AFTER #3278.
  G9  no docs/design/3340-* (per #1655), no tests/issue*/test_issue_3340.cpp
      (per #81967 — src-aligned suites only).
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text()
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3340 cross-flat provenance homology linter ===")
    macro = read("src/compiler/macro_expansion.cpp")
    test = read("tests/compiler/test_macro_cross_flat_hygiene.cpp")
    build = read("build.py")

    must("#3340" in macro, "G1: macro_expansion.cpp cites Issue #3340")
    must(
        "&target_pool != &source_pool" in macro,
        "G2: cross-pool gate (target_pool != source_pool)",
    )
    must(
        "production_defaults_active()" in macro and "g_macro_expand_sandbox_strict" in macro,
        "G3: production / force-hygienic gate (Soft/Off zero-cost)",
    )
    must(
        "target.set_provenance(cur, 0)" in macro,
        "G4: zeros leftover provenance (prefer 0 over table transplant)",
    )
    must(
        "g_hygiene_violation_in_macro_expand_total" in macro and "g_3340_" not in macro,
        "G5: reuses existing violation counter (no new metric)",
    )
    must(
        "if (target.provenance(cur) == 0)" in macro and "target.set_provenance(cur, origin)" in macro,
        "G6: clone origin stamp intact (feeds homology check)",
    )
    must("target.provenance(cloned) == 0u" in test, "G7a: AC8 provenance-zero assertion")
    must("same-pool keeps provenance copy" in test, "G7b: AC9 same-pool provenance keep")
    must("Soft keeps provenance copy" in test, "G7c: AC9 Soft provenance keep")
    must("leftover provenance zeroed" in test, "G7d: AC10 leftover provenance zeroed")
    must("Issue #3340" in test, "G7e: test cites #3340")
    must(
        "check_cross_flat_provenance_homology_3340.py" in build,
        "G8: build.py wires linter",
    )
    # AFTER #3278: 3340 block appears after the 3278 script name in build.py.
    i3278 = build.find("check_cross_flat_schema_homology_3278.py")
    i3340 = build.find("check_cross_flat_provenance_homology_3340.py")
    must(
        i3278 >= 0 and i3340 > i3278,
        "G8b: linter wired AFTER #3278",
    )
    must(
        not any(p.name.startswith("3340-") for p in (ROOT / "docs/design").glob("3340-*"))
        if (ROOT / "docs/design").exists()
        else True,
        "G9a: no docs/design/3340-* per #1655",
    )
    must(
        not (ROOT / "tests/issues" / "test_issue_3340.cpp").exists(),
        "G9b: no tests/issue*/test_issue_3340.cpp per #81967",
    )

    if failures:
        print(f"\n#3340 linter FAILED: {len(failures)} gate(s)")
        return 1
    print("\nOK #3340 cross_flat_provenance_homology: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
