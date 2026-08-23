#!/usr/bin/env python3
"""Issue #3278 linter — cross-FlatAST schema_cache / StringPool homology.

Residual from the 2026-08-22 Macro + Hygiene + Self-Evo residual-gap
analysis (main @ c11e8039): `clone_macro_body_at_depth` + rest-param
stamping copy the source node's schema_cache (a type-id index into the
SOURCE type registry) into the target by raw integer. Under concurrent
fiber steal mid-clone + densify/compact of either source or target
FlatAST, that integer can become non-homologous with the TARGET type
environment / StringPool interning — a stale id can short-circuit the
type checker's schema_cache hit path (cached_schema == tid.index,
type_checker_impl.cpp) into a wrong-type cache hit for self-evo loops
that expand macros across workspaces or after steal.

Fix (minimal, no second hygiene model): `ensure_cross_flat_expand_consistency`
re-stamps the cloned subtree against the target under production /
force-hygienic — cross-pool clones clear copied schema ids (0 = re-infer
in the target env, always safe); OOB ids (>= kSchemaIdMax, #2859 bound)
bump the EXISTING g_hygiene_violation_in_macro_expand_total (fail-closed,
Agent-visible via query:macro-provenance-stats cross-flat-violation-total).
Same-pool clones keep the #390 copy (homologous — shared registry).
Soft/Off: gate short-circuits before any walk (zero-cost contract).

Gate rows:
  G1  src/compiler/macro_expansion.cpp cites Issue #3278 in
      ensure_cross_flat_expand_consistency (the cross-flat hook).
  G2  cross-pool gate keyed on target_pool != source_pool (homology
      boundary is the StringPool / type-registry identity).
  G3  production / force-hygienic gate (production_defaults_active or
      g_macro_expand_sandbox_strict) — Soft/Off zero-cost.
  G4  OOB bound kSchemaIdMax (1<<24, #2859) — fail-closed drift signal.
  G5  reuses g_hygiene_violation_in_macro_expand_total (no new metric).
  G6  clone_macro_body_at_depth schema_cache copy site still present
      (source-cite for the #390 path that feeds the homology check).
  G7  test ACs in tests/compiler/test_macro_cross_flat_hygiene.cpp
      (the #2235 suite home — issue AC6: extend existing suite).
  G8  build.py wires this linter.
  G9  no docs/design/3278-* (per #1655), no tests/issue*/test_issue_3278.cpp
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
    print("=== #3278 cross-flat schema_cache homology linter ===")
    macro = read("src/compiler/macro_expansion.cpp")
    test = read("tests/compiler/test_macro_cross_flat_hygiene.cpp")
    build = read("build.py")

    must("#3278" in macro, "G1: macro_expansion.cpp cites Issue #3278")
    must(
        "&target_pool != &source_pool" in macro,
        "G2: cross-pool gate (target_pool != source_pool)",
    )
    must(
        "production_defaults_active()" in macro and "g_macro_expand_sandbox_strict" in macro,
        "G3: production / force-hygienic gate (Soft/Off zero-cost)",
    )
    must("kSchemaIdMax" in macro, "G4: OOB kSchemaIdMax bound (#2859)")
    must(
        "g_hygiene_violation_in_macro_expand_total" in macro,
        "G5: reuses existing violation counter (no new metric)",
    )
    must(
        "target.set_schema_cache(new_id, source.schema_cache(body_id))" in macro,
        "G6: #390 schema_cache copy site intact (feeds homology check)",
    )
    must("ac3278_cross_pool_schema_restamp" in test, "G7a: test AC8 present")
    must("ac3278_same_pool_and_soft_keep_copy" in test, "G7b: test AC9 present")
    must("ac3278_drift_fail_closed" in test, "G7c: test AC10 present")
    must("ac3278_source_cite" in test, "G7d: test AC11 present")
    must(
        "check_cross_flat_schema_homology_3278.py" in build,
        "G8: build.py wires linter",
    )
    must(
        not any(p.name.startswith("3278-") for p in (ROOT / "docs/design").glob("3278-*"))
        if (ROOT / "docs/design").exists()
        else True,
        "G9a: no docs/design/3278-* per #1655",
    )
    must(
        not (ROOT / "tests/issues" / "test_issue_3278.cpp").exists(),
        "G9b: no tests/issue*/test_issue_3278.cpp per #81967",
    )

    if failures:
        print(f"\n#3278 linter FAILED: {len(failures)} gate(s)")
        return 1
    print("\nOK #3278 cross_flat_schema_homology: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
