#!/usr/bin/env python3
"""Issue #3066: composite / lockless batch typed-audit mid == SE trail mid.

Production/Full pins one join mid for the batch and publishes it to
typed TLS + last_stamped + SE before sub-mutates. Sampled + force-reason
promotes that mid. Soft/quiet: no extra allocation.

Contract:
  AC1 Production batch: typed deny + SE share last_stamped_audit_mid
  AC2 Sampled + force-reason: joinable mid (no silent fallback diverge)
  AC3 Soft/Off: zero extra (no pin, historical mid=1 retained)
  AC4 extend test_audit_mutation_id_unify; linter; no docs/design/; no test_issue_3066

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/compiler/test_audit_mutation_id_unify.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3066", "AC1 header", tma)
    must("pin_composite_batch_join_mid", "AC1 pin", tma)
    must("join_audit_and_se_mid", "AC1 join", tma)
    must("join_audit_and_se_mid", "AC1 require_effect", sec)
    must("pin_composite_batch_join_mid", "AC1 batch pinning", ev)
    must("pin_composite_batch_join_mid", "AC1 nested/batch enter", mb)
    must("ac3066_1_production_batch_share_mid", "AC1 test", t)

    # AC2
    must("promote_sampled_force_join_mid", "AC2 promote", tma)
    must("AuditOutcome::Error", "AC2 force pin", tma)
    must("ac3066_2_sampled_force_joinable", "AC2 test", t)

    # AC3
    must("Soft/Sampled quiet", "AC3 helper", tma)
    must("ac3066_3_soft_zero_extra", "AC3 test", t)

    # AC4
    must("schema-3066", "AC4 query", q)
    must("last-composite-batch-join-mid", "AC4 join key", q)
    must("composite-audit-se-join-wired", "AC4 wired", q)
    must("check_composite_audit_mid_se_join_3066", "AC4 build", build)
    must("cmd_composite_audit_mid_se_join_3066", "AC4 cmd", build)
    must("ac3066_4_linter_no_design", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3066.cpp").is_file():
        fails.append("AC4: test_issue_3066.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3066*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3066 composite/batch typed↔SE join mid — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
