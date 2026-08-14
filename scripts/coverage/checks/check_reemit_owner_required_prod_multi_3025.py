#!/usr/bin/env python3
"""Issue #3025: production multi-eval C-ABI reemit requires owner.

Close residual after #2951 / #2845 / #2753: direct aura_reemit_aot_for_dirty
without owner must not silently reemit under production multi-eval.
Reload public fail exits stamp would_allow_native=false.

Contract (one row per AC):
  AC1  production multi-eval + no owner → return 0 + reject metric
  AC2  Soft / single-eval unchanged; public reload fail stamps proof
  AC3  service_dirty cascade still sets ReemitEvalOwnerGuard
  AC4  existing keys preserved; new counter additive + query-visible
  AC5  tests + build.py; no invent / docs/design; not a second table

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

    br = _read("src/compiler/aura_jit_bridge.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    dirty = _read("src/compiler/service_dirty.cpp")
    evq = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    named = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    rec = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3025", "AC1", br)
    must("g_reemit_owner_missing_reject_total", "AC1 counter", br)
    must("aura_aot_get_reemit_owner_eval() == nullptr", "AC1 reemit-owner", br)
    must("aura_aot_get_register_owner_eval() == nullptr", "AC1 register-owner", br)
    must("aura_aot_state_map_size() > 1", "AC1 multi-eval", br)
    must("3025 AC1", "AC1 test", named)

    # AC2
    must("Soft / Off / single-eval", "AC2 soft cite", br)
    must("note_reload_rollback", "AC2 residual stamp", br)
    must("public reload fail stamps would_allow_native=false", "AC2 residual cite", br)
    must("ac3025_reload_fail_stamps_proof", "AC2 reload test", rec)
    must("3025 AC2", "AC2 soft test", named)

    # AC3
    must("ReemitEvalOwnerGuard", "AC3 cascade", dirty)
    must("aura_aot_set_reemit_owner_eval", "AC3 owner stamp", dirty)
    must("3025 AC3", "AC3 test", named)

    # AC4
    must("schema-3025", "AC4 schema", evq)
    must("reemit-owner-missing-reject-total", "AC4 query", evq)
    must("reemit-owner-missing-reject-wired", "AC4 wired", evq)
    must("schema-2951", "AC4 preserve 2951", evq)
    must("reemit_owner_missing_reject_total_v_read", "AC4 v_read", hdr)
    must("reemit_owner_missing_reject_total_v_read", "AC4 stub", stub)
    must("3025 AC4", "AC4 test", named)

    # AC5
    must("check_reemit_owner_required_prod_multi_3025", "AC5 build", build)
    must("cmd_reemit_owner_required_prod_multi_3025", "AC5 build cmd", build)
    must("ac3025_1_prod_multi_no_owner_rejects", "AC5 AC1 test", named)
    if "AgentRegistry" in br[br.find("Issue #3025") : br.find("Issue #3025") + 2500]:
        fails.append("AC5: must not introduce AgentRegistry")
    if (ROOT / "tests" / "compiler" / "test_issue_3025.cpp").is_file():
        fails.append("AC5: test_issue_3025.cpp present (forbidden per #81967)")
    if _read("docs/design/3025-reemit-owner-required.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3025 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3025 production C-ABI reemit owner required — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
