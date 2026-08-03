#!/usr/bin/env python3
"""Issue #2618: production smoke hard-asserts residual_aos_bridge_total == 0.

Contract:
  AC1 Production smoke fails if residual_aos_bridge_total != 0 (under SoA-only)
  AC2 Smoke exercises SoA path (soa_only_path_total advanced)
  AC3 Explicit test opt-in bridge still works in dedicated test jobs
  AC4 Source-cite #2520 / schema-2520 / #2618 in assert path
  AC5 No false fail when AURA_ALLOW_AOS_BRIDGE intentionally set (soft skip)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import os
import re
import subprocess
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
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    low = _read("src/compiler/lowering_impl.cpp")
    test = _read("tests/compiler/test_soa_residual_production_smoke_2618.cpp")
    dual = _read("tests/compiler/test_ir_soa_dual_emit_batch.cpp")
    ban = _read("tests/compiler/test_soa_ban_residual_aos_bridge_2520.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — production smoke residual hard-assert
    must("#2618", "AC1", soa)
    must("kSchemaResidualAosProductionSmoke", "AC1", soa)
    must("residual_aos_bridge_total==0", "AC1", soa)
    must("ac1_production_smoke_residual_zero", "AC1", test)
    must("residual_aos_bridge_total", "AC1", test)
    must("FATAL", "AC1", test)  # loud fail message

    # AC2 — SoA path exercised
    must("g_soa_only_path_total_atomic", "AC2", low)
    must("soa_only_path", "AC2", test)
    must("ac2_soa_path_exercised", "AC2", test)
    must("soa-only-path-total", "AC2", q)

    # AC3 — test opt-in still works (dedicated jobs / dual-emit)
    must("set_allow_aos_bridge_for_test", "AC3", soa)
    must("set_allow_aos_bridge_for_test(true)", "AC3", dual)
    must("ac3_test_opt_in_still_works", "AC3", test)
    must("ac4_test_opt_in", "AC3", ban)  # lineage #2520

    # AC4 — source-cite schema-2520 / #2618
    must("schema-2520", "AC4", q)
    must("schema-2618", "AC4", q)
    must("kSchemaResidualAosBan", "AC4", soa)
    must("#2520", "AC4", test)
    must("schema-2520", "AC4", test)
    must("ac4_source_cite", "AC4", test)

    # AC5 — soft skip when bridge intentionally allowed
    must("AURA_ALLOW_AOS_BRIDGE", "AC5", test)
    must("ac5_soft_allow_no_false_fail", "AC5", test)
    must("aos_bridge_allowed", "AC5", test)

    # Gate / cmake / build wiring
    must("test_soa_residual_production_smoke_2618", "gate", cmake)
    must("check_soa_residual_production_smoke_2618", "gate", build)
    must("cmd_soa_residual_production_smoke_coverage", "gate", build)
    must("soa_residual_production_smoke_wired", "gate", soa)
    must("soa-residual-production-smoke-wired", "gate", q)

    # Grep: production TUs still limited (retain #2520 ban surface)
    must("check_soa_ban_residual_aos_bridge_2520", "retain", build)

    for rel in (
        "docs/design/soa_residual_production_smoke_2618.md",
        "docs/soa_residual_production_smoke_2618.md",
        "design/2618.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    # Optional: if production smoke binary is built, run it under production env
    # (AURA_ALLOW_AOS_BRIDGE unset). Soft configs with env=1 are not run here.
    for build_dir in ("build", "build_asan", "build_release"):
        binary = ROOT / build_dir / "test_soa_residual_production_smoke_2618"
        if not binary.is_file():
            continue
        env = os.environ.copy()
        env.pop("AURA_ALLOW_AOS_BRIDGE", None)
        env["AURA_ALLOW_AOS_BRIDGE"] = "0"  # explicit deny for production smoke
        r = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if r.returncode != 0:
            fails.append(
                f"AC1: production smoke binary failed under SoA-only "
                f"(exit {r.returncode}; schema-2520/#2618). stderr:\n{r.stderr[-2000:]}"
            )
        else:
            out = (r.stdout or "") + (r.stderr or "")
            if "residual" not in out.lower() and "2618" not in out:
                # Still OK if binary is quiet-success
                pass
            if re.search(r"\bfailed\b", out, re.I) and "0 failed" not in out:
                fails.append("AC1: production smoke reported failures in output")
        break  # one binary is enough

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2618 residual production smoke — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
