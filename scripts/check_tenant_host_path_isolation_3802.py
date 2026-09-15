#!/usr/bin/env python3
"""Issue #3802: EXEMPT_2ARG write-file/sys-* tenant host-path isolation.

Under Restricted+MT / Strict, path-taking host IO (write-file / sys-open /
sys-write siblings) resolve under a tenant root derived from
capability_tenant_id_. Cross-tenant path escape → IsolationDeny SE
(tenant-path-escape), zero write. Soft/Off / single-tenant Restricted
passthrough. EXEMPT_2ARG inventory size stays 5 (no NodeId redesign).

Contract (one row per AC):
  AC1  Restricted+MT tenant A cannot write under tenant B prefix; deny + SE
  AC2  Soft/Off / single-tenant Restricted keep passthrough
  AC3  EXEMPT_2ARG inventory size stable; no new query key
  AC4  cite-first linter + dual-tenant chaos oracle

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    hh = _read("src/compiler/tenant_host_path.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    filep = _read("src/compiler/evaluator_primitives_file.cpp")
    iop = _read("src/compiler/evaluator_primitives_io.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    mandate = _read("scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py")

    # ── AC1 ──
    must("Issue #3802", "AC1 cite hh", hh)
    must("tenant-path-escape", "AC1 SE reason", hh)
    must("resolve_tenant_host_path", "AC1 resolve helper", hh)
    must("check_tenant_host_path", "AC1 Evaluator API", ixx)
    must("check_tenant_host_path", "AC1 impl", sec)
    must("IsolationDeny", "AC1 SE kind", sec)
    must("tenant-path-escape", "AC1 SE reason emit", sec)
    must("check_tenant_host_path", "AC1 write-file", filep)
    must("3802 AC1", "AC1 test", test)

    # ── AC2 Soft/Off / single-tenant Restricted ──
    must("Passthrough", "AC2 verdict", hh)
    must("tenant_host_path_policy_active", "AC2 gate", hh)
    must("single-tenant Restricted", "AC2 cite", hh)
    must("3802 AC2", "AC2 test", test)
    must("Soft/Off", "AC2 Soft test", test)

    # ── AC3 inventory stable ──
    must("kResidualNodeIdExemptOpsCount = 5", "AC3 exempt count ixx", ixx)
    must("kNodeIdMandateExemptOpsCount = 5", "AC3 mandate count ixx", ixx)
    # EXEMPT_2ARG still size 5 in mandate linter
    if "len(EXEMPT_2ARG_OPS) != 5" not in mandate and "expected 5" not in mandate:
        fails.append("AC3: mandate linter no longer pins EXEMPT_2ARG size 5")
    must_not("schema-3802", "AC3 no schema key", prim)
    must_not("issue-3802", "AC3 no issue key", prim)
    must("3802 AC3", "AC3 test", test)

    # ── AC4 cite-first + chaos ──
    must("check_tenant_host_path_isolation_3802", "AC4 build.py", build)
    must("Issue #3802", "AC4 build cite", build)
    must("check_tenant_host_path", "AC4 sys-open/io", iop)
    must("3802 AC4", "AC4 test", test)
    must("chaos", "AC4 chaos oracle", test.lower())
    for rel in ("tests/core/test_issue_3802.cpp", "tests/compiler/test_issue_3802.cpp"):
        if _read(rel):
            fails.append(f"AC4: {rel} exists — forbidden per #81967")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3802-*.md"):
            fails.append(f"AC4: {f.relative_to(ROOT)} exists — forbidden per #1655")

    # write-file / sys-open must call check_tenant_host_path (cite-first)
    if not re.search(r'add\("write-file"[\s\S]{0,2000}?check_tenant_host_path', filep):
        fails.append("AC4: write-file body missing check_tenant_host_path within window")
    if not re.search(r'"sys-open"[\s\S]{0,2000}?check_tenant_host_path', iop):
        fails.append("AC4: sys-open body missing check_tenant_host_path within window")

    if fails:
        print(f"Issue #3802 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3802 tenant host-path isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
