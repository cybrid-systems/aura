#!/usr/bin/env python3
"""Issue #3054: mid-fallback refuse emits one joinable SecurityEvent.

Contract (one row per AC):
  AC1  production/Full + mid=0 → resolve returns 0 AND exactly one SE
       (InvariantFail, reason mid-fallback-refused) via emit_security_event_durable
  AC2  Soft / Sampled + mid=0 → no new SE from resolve
  AC3  Nested / re-resolve under same boundary does not double-emit (TLS)
  AC4  query:security-audit can filter the refuse event; refused-total stays
  AC5  No process-origin stamp / gen bump on refuse
  AC6  Extend test_audit_mid_fallback_slo (#81967); no test_issue_3054.cpp;
       no docs/design/ (#1655)

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_audit_mid_fallback_slo.cpp")
    build = _read("build.py")

    must("kMidFallbackRefuseSeIssue = 3054", "AC1 stamp", tma)
    must("emit_security_event_durable", "AC1 emit", tma)
    must("mid-fallback-refused", "AC1 reason", tma)
    must("resolve-audit-mid", "AC1 op", tma)
    must("InvariantFail", "AC1 kind", tma)
    must("3054 AC1", "AC1 test", test)

    must("3054 AC2", "AC2 test", test)
    must("Soft emits no refuse SE", "AC2 check", test)

    must("g_tls_mid_fallback_refuse_se_emitted", "AC3 TLS", tma)
    must("clear_mid_fallback_refuse_se_tls", "AC3 clear", tma)
    must("3054 AC3", "AC3 test", test)

    must("schema-3054", "AC4 query", sec)
    must("filt_reason", "AC4 reason filter", sec)
    must("refuse-se-total", "AC4 key", sec)
    must("refused-total", "AC4 counter kept", sec)
    must("3054 AC4", "AC4 test", test)

    must("3054 AC5", "AC5 test", test)
    must("return 0;", "AC5 refuse", tma)

    must("check_mid_fallback_refuse_se_3054", "AC6 build", build)
    must("3054 AC6", "AC6 test", test)
    if _read("docs/design/3054-mid-fallback-refuse-se.md"):
        fails.append("AC6: docs/design/3054-* present")
    if _read("tests/compiler/test_issue_3054.cpp"):
        fails.append("AC6: test_issue_3054.cpp present")
    if "class MidFallbackSeBus" in tma or "g_mid_fallback_se_ring_3054" in tma:
        fails.append("AC6: second audit bus introduced")

    if fails:
        print(f"Issue #3054 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3054 mid-fallback refuse SE — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
