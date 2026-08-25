#!/usr/bin/env python3
"""Issue #3295: dirty-only OwnershipEnv re-sim forces full walk under Production.

Residual of #2460/#3006: infer_flat_partial's dirty-only validate can miss
cross-function / closure-captured linear flows when the dirty set omits
callee locals. Production/Full forces validate_ownership_full when the
escape gate or densify-pending is present; Soft observes only. Quiet:
no escape / no densify-pending → one escape-gate load + one densify load.

Contract:
  AC1 Production + escape/densify → force full walk, boundary denies
  AC2 Soft observe only; quiet zero extra
  AC3 lineage #2460/#3006/#2964 retained
  AC4 extend test_linear_partial_revalidate; this linter; no invent / docs

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    tci = _read("src/compiler/type_checker_impl.cpp")
    _read("src/compiler/ownership_escape_lowering_gate.h")
    t = _read("tests/compiler/test_linear_partial_revalidate.cpp")
    build = _read("build.py")

    must("kLinearForceFullValidateIssue = 3295", "AC1 stamp", aud)
    must("linear_force_full_validate_needed", "AC1 helper", aud)
    must("aura_escape_move_gate_active", "AC1 escape arm", aud)
    must("linear_densify_scan_mismatch_inject_pending", "AC1 densify arm", aud)
    must("g_linear_force_full_validate_total", "AC1 hard counter", aud)
    must("linear_force_full_validate_needed", "AC1 impl call", tci)
    must("validate_ownership_full", "AC1 impl full walk", tci)
    must("ac3295_1_production_escape_forces_full", "AC1 test", t)

    must("g_linear_force_full_validate_observe_total", "AC2 soft counter", aud)
    must("ac3295_2_soft_observe_only", "AC2 test", t)
    h = aud.find("linear_force_full_validate_needed")
    if h < 0:
        fails.append("AC2: helper missing")
    else:
        end = aud.find("}", h)
        body = aud[h:end] if end > h else ""
        if "if (!escape_or_densify)" not in body:
            fails.append("AC2: quiet escape/densify check first")
        if "production_defaults_active" not in body:
            fails.append("AC2: production gate in helper")

    must("Issue #2460", "AC3 impl #2460", tci)
    must("kLinearFastPathDirtyRevalidateIssue", "AC3 #3006", aud)
    must("linear_fast_path_ok", "AC3 #2964", aud)
    must("ac3295_3_lineage", "AC3 test", t)
    if "g_3295_" in aud or "g_3295_" in tci:
        fails.append("AC3: invented g_3295_* counter")

    must("check_linear_force_full_validate_3295", "AC4 build.py", build)
    must("ac3295_4_source_linter", "AC4 test", t)
    must("Issue #3295", "AC4 impl cite", tci)
    if (ROOT / "tests" / "compiler" / "test_issue_3295.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3295.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3295.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3295.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3295-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3295 linear_force_full_validate:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3295 linear_force_full_validate: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
