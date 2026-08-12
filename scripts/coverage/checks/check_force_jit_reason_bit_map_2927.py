#!/usr/bin/env python3
"""Issue #2927: AotReloadFail → force_jit_regions_mask stable bit groups.

Residual of #2845/#2232/#2302: enum-ordinal bits collapsed demotion groups
so Agents could not heal Env-only drift without demoting Version-healthy
regions.

Contract (one row per AC):
  AC1 Env exhaust → only Env group bit (bit 1); Version bit clear
  AC2 Linear → bit 2 only; ConsistencyProof mask matches registry
  AC3 soft/success → no mask change; stamped_on_fail only on fail path
  AC4 query keys force-jit-reason-bit-map-wired + last-mapped-bit
  AC5 source-cite + this linter; extend test_reload_recovery_query;
      no invent test; no docs/design/*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    bridge_h = _read("src/compiler/aura_jit_bridge.h")
    reg = _read("src/compiler/hot_update_registry.cpp")
    reg_h = _read("src/compiler/hot_update_registry.hh")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1 — stable reason→bit SSOT
    must("aot_reload_fail_to_force_jit_mask", "AC1", bridge_h)
    must("aot_reload_fail_to_force_jit_bit_index", "AC1", bridge_h)
    must("#2927", "AC1", bridge_h)
    must("AotReloadFail::Env", "AC1", bridge_h)
    must("AotReloadFail::Linear", "AC1", bridge_h)
    # Group table documented
    must("Version | Defuse", "AC1 table", bridge_h)
    must("Region | Staging", "AC1 table", bridge_h)
    must("Dlopen | Other", "AC1 table", bridge_h)

    # AC2 — on_force_jit uses mapper; proof stamp gets live mask
    must("aot_reload_fail_to_force_jit_mask", "AC2", reg)
    must("stamp_aot_reload_consistency_proof_fail_after_force_jit", "AC2", reg)
    must("#2927", "AC2", reg)
    # Must not use enum ordinal as bit (1 << reason)
    ofj = reg.find("void HotUpdateRegistry::on_force_jit_for_reason")
    if ofj < 0:
        fails.append("AC2: on_force_jit_for_reason not found")
    else:
        body = reg[ofj : ofj + 2500]
        if "aot_reload_fail_to_force_jit_mask" not in body:
            fails.append("AC2: on_force_jit body missing mapped mask")
        # Ban enum-ordinal fetch_or (1 << reason); mapped mask uses bit_mask var.
        if "fetch_or(static_cast<std::uint64_t>(1)" in body:
            fails.append("AC2: still fetch_or (1 << reason) enum ordinal")

    # on_exhausted_min_dirty_queue shares encoding
    exh = reg.find("void HotUpdateRegistry::on_exhausted_min_dirty_queue")
    if exh < 0:
        fails.append("AC2: on_exhausted_min_dirty_queue not found")
    else:
        body = reg[exh : exh + 800]
        if "aot_reload_fail_to_force_jit_mask" not in body:
            fails.append("AC2: exhausted min-dirty must use mapped mask")

    # AC3 — soft path documents zero extra work (no stamp on success clear)
    must("#2927", "AC3 docs", reg_h)
    must("aot_reload_fail_to_force_jit_mask", "AC3", reg_h)

    # AC4 — query keys
    must("force-jit-reason-bit-map-wired", "AC4", mut)
    must("last-mapped-bit", "AC4", mut)
    must("schema-2927", "AC4", mut)
    must("last_force_jit_mapped_bit", "AC4", reg_h)
    must("force_jit_reason_bit_map_wired", "AC4", reg_h)
    must("schema_2927", "AC4 snapshot", reg_h)

    # AC5 — tests + wire + no invent/design
    must("ac2927_1_env_only_bit", "AC5", test)
    must("ac2927_2_linear_and_proof_match", "AC5", test)
    must("ac2927_3_soft_success_no_mask_change", "AC5", test)
    must("ac2927_4_query_keys", "AC5", test)
    must("ac2927_5_source_and_linter", "AC5", test)
    must("check_force_jit_reason_bit_map_2927", "AC5", build)
    must("ac2845_2_force_jit_mask_in_proof", "AC5 #2845 preserved", test)
    if (ROOT / "tests" / "compiler" / "test_issue_2927.cpp").is_file():
        fails.append("AC5: test_issue_2927.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2927*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2927 AotReloadFail → force_jit group bits — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
