#!/usr/bin/env python3
"""Issue #2835: production Restricted + multi-tenant → hard_fiber_isolation.

Contract (one row per AC):
  AC1 multi_tenant arms hard without forcing Strict; Restricted+multi hard
  AC2 pure Restricted single-tenant soft (#2536)
  AC3 AURA_HARD_FIBER_ISOLATION=0 forces soft under multi Restricted
  AC4 multi + K=64 retain; Soft/off hard=false K=0
  AC5 query schema-2835 + capability-hard-fiber keys
  AC6 test extension + linter; no docs/design/2835-*

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

    sec = _read("src/compiler/security_defaults.hh")
    obs = _read("src/compiler/evaluator_primitives_security.cpp")
    jit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_hard_fiber_restricted.cpp")
    build = _read("build.py")

    # AC1 — multi_tenant arms hard; no force Strict for multi alone
    must("Issue #2835", "AC1", sec)
    must("hard_default = multi_tenant", "AC1", sec)
    # Must not force set_mode(Strict) solely for multi_tenant arming.
    # Allow historical comments; ban live assignment after multi_tenant = true
    # without Restricted latency note.
    if "set_mode(SandboxMode::Strict)" in sec:
        # Still OK if only in comments or other paths — ensure multi block
        # does not assign Strict after multi_tenant = true without #2835 note.
        pos = sec.find("multi_tenant = true")
        window = sec[pos : pos + 400] if pos >= 0 else ""
        if "set_mode(SandboxMode::Strict)" in window:
            fails.append("AC1: multi_tenant still forces set_mode(Strict)")

    # AC2 — pure Restricted soft lineage present
    must("2536", "AC2", sec)
    must("soft share", "AC2", sec)

    # AC3 — explicit HFI off
    must("hfi_explicit_off", "AC3", sec)
    must("AURA_HARD_FIBER_ISOLATION", "AC3", sec)

    # AC4 — K=64 multi retain still armed
    must("kDefaultGrantEpochRetainWindowMultiTenant", "AC4", sec)
    must("multi_tenant || strict", "AC4", sec)

    # AC5 — query surface
    must("schema-2835", "AC5", obs)
    must("restricted-multi-tenant-hard-fiber-wired", "AC5", obs)
    must("schema-2835", "AC5", jit)
    must("capability-hard-fiber-isolation", "AC5", jit)

    # AC6 — test + linter + no design docs
    must("ac2835", "AC6", test)
    must("2835", "AC6", test)
    must("AURA_MULTI_TENANT", "AC6", test)
    must("check_restricted_multi_tenant_hard_fiber_2835", "AC6", build)
    if not (ROOT / "tests" / "compiler" / "test_hard_fiber_restricted.cpp").is_file():
        fails.append("AC6: missing test_hard_fiber_restricted.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2835.cpp").is_file():
        fails.append("AC6: test_issue_2835.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2835-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2835 Restricted multi-tenant hard_fiber escalate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
