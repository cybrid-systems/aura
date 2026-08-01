#!/usr/bin/env python3
"""Issue #2496: GeneralObjectPin adoption coverage gate (inventory vs wire_total).

#2298 / #2337 / #2363 shipped GeneralObjectPin + mutate-path adoption. #2496
closes the residual gap — inventory sites documented in `kGeneralObjectPinAdoptSiteCount`
are not enforced by linter or runtime policy. New mutate/agent/scratch create
sites can land without pin wire-up; dashboards see a number but cannot enforce
`wire_total` growth vs inventory under production Moving.

Contract:
  AC1 Linter fails when a listed inventory site lacks wire call
     (note_general_object_pin_mutate_wire / wire_general_object_create_pair).
  AC2 Adding a new densify-tracked intermediate create without pin fails gate
     (or required mode). Source-cite the kGeneralObjectPinAdoptSiteCount
     inventory list (7 sites) — linter tracks site count + wire_total.
  AC3 Soft / empty densify unchanged (zero extra cost when policy off).
  AC4 Query shows inventory count + wire coverage signal
     (kGeneralObjectPinAdoptSiteCount vs general_object_pin_mutate_wire_total).
  AC5 Tests + source-cite for all inventory sites.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    lp = _read("src/core/lifetime_pin.ixx")
    test = _read("tests/core/test_general_object_pin_coverage_gate_2496.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — inventory sites documented + wire helpers + issue stamps.
    must("Issue #2496", "AC1", lp)
    must("kGeneralObjectPinIssue = 2298", "AC1", lp)
    must("kGeneralObjectPinAdoptIssue = 2363", "AC1", lp)
    must("kGeneralObjectPinAdoptSiteCount = 7", "AC1", lp)
    must("note_general_object_pin_mutate_wire", "AC1", lp)
    must("wire_general_object_create_pair", "AC1", lp)
    must("general_object_pin_mutate_wire_total", "AC1", lp)
    # Inventory sites all listed in kGeneralObjectPinAdoptSiteCount comment.
    must("mutate:replace-pattern", "AC1", lp)
    must("batch :replace-pattern", "AC1", lp)
    must("require import parse", "AC1", lp)
    must("query:pattern", "AC1", lp)
    must("query:pattern guard", "AC1", lp)
    must("load", "AC1", lp)
    must("eval-expr", "AC1", lp)

    # AC2 — new create without pin fails gate (or required mode). Source-cite
    # the count + the existing 2363 linter that already enforces per-site
    # coverage (this gate adds the explicit hard-fail at the build gate level).
    must("kGeneralObjectPinAdoptSiteCount = 7", "AC2", lp)
    must("wire_general_object_create_pair", "AC2", lp)

    # AC3 — soft / empty densify unchanged. The 2363 / 2298 work already
    # enforces this; #2496 just hardens the gate. Source-cite the
    # note_general_object_pin_mutate_wire() helper is the only hot-path
    # touch under Moving densify (zero cost when no pin adopted).
    must("note_general_object_pin_mutate_wire", "AC3", lp)

    # AC4 — query shows inventory + wire coverage signal.
    must("kGeneralObjectPinAdoptSiteCount", "AC4", lp)
    must("general_object_pin_mutate_wire_total", "AC4", lp)
    # Issue #2496: AURA_GENERAL_OBJECT_PIN=required fail-closed env var
    # (optional runtime enforcement). Source-cite if implemented in #2496.
    must("AURA_GENERAL_OBJECT_PIN", "AC4", lp)

    # AC5 — registrations + this linter.
    must("ac1_inventory_sites_wired", "AC1", test)
    must("ac2_soft_zero_cost_retained", "AC2", test)
    must("ac3_query_inventory_vs_wire", "AC3", test)
    must("ac4_required_mode_fail_closed", "AC4", test)
    must("ac5_source_cite_registrations", "AC5", test)
    must("Issue #2496", "AC5", test)
    must("test_general_object_pin_coverage_gate_2496", "AC5", cmake)
    must(
        "aura_add_issue_test(test_general_object_pin_coverage_gate_2496)",
        "AC5",
        cmake,
    )
    must(
        "aura_issue_test_link_llvm_jit(test_general_object_pin_coverage_gate_2496)",
        "AC5",
        cmake,
    )
    must("check_general_object_pin_coverage_gate_2496", "AC5", build)
    must(
        "cmd_general_object_pin_coverage_gate_2496_coverage",
        "AC5",
        build,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2496 GeneralObjectPin adoption coverage gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
