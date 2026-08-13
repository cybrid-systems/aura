#!/usr/bin/env python3
"""Issue #2943: production multi-tenant / Strict → hard_fiber_isolation=true.

Closes residual soft grant-fiber share under pure Strict (after #2835
multi-tenant-only arming). Complements #2883 resume principal hard deny.

Contract (one row per AC):
  AC1  multi-tenant OR Strict → set_hard_fiber_isolation(true)
  AC2  AURA_HARD_FIBER_ISOLATION=0|1 env override preserved
  AC3  Soft / Off / pure Restricted single-tenant → soft default
  AC4  hard deny path bumps capability_fiber_hard_deny_total (lineage)
  AC5  Additive schema-2943 + production-hard-fiber-default-wired
  AC6  Source-cite + tests; no invent/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
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

    sec = _read("src/compiler/security_defaults.hh")
    cap = _read("src/core/capability_model.hh")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    jit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_hard_fiber_restricted.cpp")
    test_su = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    # ── AC1: multi OR strict arms hard ──
    must("Issue #2943", "AC1", sec)
    must("strict_sandbox", "AC1", sec)
    # hard_default = multi_tenant || strict_sandbox (or equivalent)
    if (
        not re.search(
            r"hard_default\s*=\s*multi_tenant\s*\|\|\s*strict",
            sec,
        )
        and "multi_tenant || strict" not in sec
    ):
        fails.append("AC1: hard_default must be multi_tenant || strict (Strict alone hard)")
    must("set_hard_fiber_isolation(hard_default)", "AC1", sec)
    # Must not leave "Strict alone → hard=false" residual comment as live contract.
    if "Strict alone (no multi) → hard=false" in sec:
        fails.append("AC1: residual 'Strict alone → hard=false' comment still present")

    # ── AC2: env override ──
    must("AURA_HARD_FIBER_ISOLATION", "AC2", sec)
    must("hfi_explicit_off", "AC2", sec)
    # Env=0 forces soft under Strict (tested in hard_fiber_restricted)
    must("AURA_HARD_FIBER_ISOLATION", "AC2", test)
    if "HFI=0" not in test and '"0"' not in test:
        fails.append("AC2: test must cover HFI=0 soft override under Strict")

    # ── AC3: Soft / Off / pure Restricted soft ──
    must("dev_off", "AC3", sec)
    must("set_hard_fiber_isolation(false)", "AC3", sec)
    must("2536", "AC3", sec)
    must("soft share", "AC3", sec)
    # Pure Restricted soft still tested
    must("Restricted soft default", "AC3", test)

    # ── AC4: hard deny metric lineage ──
    must("capability_fiber_hard_deny_total", "AC4", cap)
    must("hard_fiber_isolation", "AC4", cap)
    # provenance_ok hard path still present
    must("fiber_hard_deny", "AC4", cap)

    # ── AC5: additive schema ──
    must("schema-2943", "AC5", posture)
    must("issue-2943", "AC5", posture)
    must("production-hard-fiber-default-wired", "AC5", posture)
    must("schema-2943", "AC5", jit)
    must("production-hard-fiber-default-wired", "AC5", jit)
    # Lineage preserved
    must("schema-2835", "AC5", posture)
    must("schema-2688", "AC5", jit)
    must("schema-2151", "AC5", posture)
    must("kProductionHardFiberDefaultIssue", "AC5", cap)

    # ── AC6: tests + linter + no invent/design ──
    must("2943", "AC6", test)
    must("ac2943", "AC6", test)
    must("check_production_hard_fiber_default_2943", "AC6", build)
    # Optional cross-cite in single_use lineage suite
    if "2943" not in test_su and "2943" not in test:
        fails.append("AC6: no 2943 tests in hard_fiber_restricted lineage")
    if (ROOT / "tests" / "compiler" / "test_issue_2943.cpp").is_file():
        fails.append("AC6: test_issue_2943.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2943-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check #2835 still green
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_restricted_multi_tenant_hard_fiber_2835.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"check_restricted_multi_tenant_hard_fiber_2835 regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2943 production multi-tenant/Strict hard_fiber_isolation default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
