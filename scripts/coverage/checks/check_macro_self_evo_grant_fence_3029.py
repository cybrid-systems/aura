#!/usr/bin/env python3
"""Issue #3029: grant_macro_self_evo TenantAdmin fence + stable limit reasons.

Contract (one row per AC):
  AC1  grant_macro_self_evo under Restricted/Strict requires TenantAdmin
  AC2  Soft/Off zero-cost allow
  AC3  ceiling/depth/pass set hygiene-gensym-ceiling / hygiene-depth-limit /
       hygiene-pass-limit; pass-limit tries checkpoint restore
  AC4  tests + build.py; no invent / docs/design; no new query:* name

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

    cap = _read("src/core/capability_model.hh")
    mx = _read("src/compiler/macro_expansion.cpp")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    q = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    grant_t = _read("tests/compiler/test_grant_macro_self_evo_stamp.cpp")
    lim_t = _read("tests/compiler/test_macro_hygiene_limits.cpp")
    build = _read("build.py")

    # AC1 grant fence
    must("Issue #3029", "AC1", cap)
    must("macro-self-evo-grant-needs-tenant-admin", "AC1 reason", cap)
    must("capability_macro_self_evo_grant_deny_total", "AC1 counter", cap)
    must("TenantAdmin", "AC1 admin", cap)
    gpos = cap.find("void grant_macro_self_evo")
    gwin = cap[gpos : gpos + 3500] if gpos >= 0 else ""
    must("sandbox_mode", "AC1 mode", gwin)
    must("has_admin", "AC1 helper", gwin)
    must("3029 AC1", "AC1 test", grant_t)

    # AC2 Soft
    must("Soft/Off", "AC2 cite", cap)
    must("3029 AC3", "AC2 soft test", grant_t)

    # AC3 reasons
    must("hygiene-gensym-ceiling", "AC3 ceiling", mx)
    must("hygiene-depth-limit", "AC3 depth", mx)
    must("hygiene-pass-limit", "AC3 pass", mx)
    must("g_macro_hygiene_last_limit_reason", "AC3 atomic", mx)
    must("aura_evaluator_try_restore_macro_expand_checkpoint", "AC3 restore", mx)
    must("aura_evaluator_try_restore_macro_expand_checkpoint", "AC3 restore impl", fib)
    must("schema-3029", "AC3 query", q)
    must("last-hygiene-limit-reason", "AC3 query key", q)
    must("schema-3029", "AC3 posture", sec)
    must("3029: last reason hygiene-depth-limit", "AC3 depth test", lim_t)
    must("3029: last reason hygiene-gensym-ceiling", "AC3 ceiling test", lim_t)
    must("3029: last reason hygiene-pass-limit", "AC3 pass test", lim_t)
    must("macro-self-evo-grant-needs-tenant-admin", "AC1 audit test", grant_t)

    # AC4 wiring
    must("check_macro_self_evo_grant_fence_3029", "AC4 build", build)
    must("cmd_macro_self_evo_grant_fence_3029", "AC4 cmd", build)
    cite = cap.find("Issue #3029")
    if cite >= 0 and "AgentRegistry" in cap[cite : cite + 2000]:
        fails.append("AC4: must not introduce AgentRegistry")
    if (ROOT / "tests" / "compiler" / "test_issue_3029.cpp").is_file():
        fails.append("AC4: test_issue_3029.cpp present (forbidden per #81967)")
    if _read("docs/design/3029-macro-self-evo-grant-fence.md"):
        fails.append("AC4: docs/design/ exists — forbidden per #1655")
    if "query:hygiene-limit-reason" in q:
        fails.append("AC4: new top-level query key (forbidden)")

    if fails:
        print(f"Issue #3029 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3029 grant fence + stable limit reasons — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
