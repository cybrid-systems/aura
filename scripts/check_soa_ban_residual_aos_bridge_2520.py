#!/usr/bin/env python3
"""Issue #2520: production path bans residual to_aos_view / AoS bridge.

Contract:
  AC1 AURA_IR_SOA_ONLY + hard-fail without aos_bridge_allowed
  AC2 residual_aos_bridge target 0; dual-emit gated
  AC3 DirtyAware/columnar prefer SoA (no hot-path materialize)
  AC4 test opt-in set_allow_aos_bridge_for_test / AURA_ALLOW_AOS_BRIDGE
  AC5 residual metric test-only; schema-2520; gate wiring
  Grep: production TUs do not call to_aos_view outside ir_soa + pass_impls

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    soa = _read("src/compiler/ir_soa.ixx")
    # Issue #2524: SoAtoAoS / DeadCoercion bodies live in pass_impls.ixx
    pm = _read("src/compiler/pass_impls.ixx") + _read("src/compiler/pass_manager.ixx")
    low = _read("src/compiler/lowering_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_soa_ban_residual_aos_bridge_2520.cpp")
    dual = _read("tests/compiler/test_ir_soa_dual_emit_batch.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2520", "AC1", soa)
    must("aos_bridge_allowed", "AC1", soa)
    must("set_allow_aos_bridge_for_test", "AC1", soa)
    must("AURA_ALLOW_AOS_BRIDGE", "AC1", soa)
    must("std::abort()", "AC1", soa)
    must("#define AURA_IR_SOA_ONLY 1", "AC1", soa)
    must("aos_bridge_allowed", "AC1", pm)
    must("ac1_production_ban", "AC1", test)

    # AC2
    must("g_residual_aos_bridge_total_atomic", "AC2", soa)
    must("&& !AURA_IR_SOA_ONLY", "AC2", low)
    must("g_soa_only_path_total_atomic", "AC2", low)
    must("ac2_residual_zero", "AC2", test)

    # AC3
    must("residual_aos_bridge_total stays 0", "AC3", pm)
    must("run_dirty", "AC3", pm)
    must("ac3_columnar_no_bridge", "AC3", test)

    # AC4
    must("set_allow_aos_bridge_for_test(true)", "AC4", dual)
    must("ac4_test_opt_in", "AC4", test)

    # AC5
    must("kResidualAosBridgeTestOnly", "AC5", soa)
    must("schema-2520", "AC5", q)
    must("residual-aos-bridge-test-only", "AC5", q)
    if "TEST-ONLY" not in met and "#2520" not in met:
        fails.append("AC5: metrics missing TEST-ONLY / #2520 residual note")
    must("test_soa_ban_residual_aos_bridge_2520", "AC5", cmake)
    must("check_soa_ban_residual_aos_bridge_2520", "AC5", build)
    must("cmd_soa_ban_residual_aos_bridge_coverage", "AC5", build)
    must("ac5_observability", "AC5", test)

    # Grep: production call sites of to_aos_view( (ignore // comments)
    allowed = {
        "src/compiler/ir_soa.ixx",
        "src/compiler/pass_manager.ixx",  # facade only
        "src/compiler/pass_impls.ixx",  # SoAtoAoSBridgePass (#2524)
    }
    src_root = ROOT / "src"
    call_re = re.compile(r"\bto_aos_(?:view|module)\s*\(")
    for path in src_root.rglob("*"):
        if path.suffix not in {".cpp", ".ixx", ".h", ".hh"}:
            continue
        rel = str(path.relative_to(ROOT))
        if rel in allowed:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for i, line in enumerate(text.splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith("//"):
                continue
            if call_re.search(line):
                fails.append(f"grep: production call site {rel}:{i} invokes to_aos_view/to_aos_module")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2520 ban residual AoS bridge — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
