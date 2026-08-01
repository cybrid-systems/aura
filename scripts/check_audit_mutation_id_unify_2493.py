#!/usr/bin/env python3
"""Issue #2493: unify mutation_id source — WorkspaceEpoch Mutation over
independent audit gens. resolve_audit_mutation_id() enforces preference
order (caller mid → current_mutation_epoch → ResourceQuota host mid →
last-resort audit gen + fallback counter bump).

Contract:
  AC1 Caller mid > current_mutation_epoch() > ResourceQuota host mid > gen
  AC2 ResourceQuota host mid used as fallback after caller + epoch miss
  AC3 AOT hot-update audit adopts resolve preference (epoch preferred)
  AC4 Soft / no-mutation-activity fallback bumps audit_mid_fallback_gen_total
  AC5 capture_security_correlated_audit uses resolve + epoch fallback
  AC6 Source-cite + tests + CMake + build.py gate + this linter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    test = _read("tests/compiler/test_audit_mutation_id_unify_2493.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1/AC2/AC3 — resolve_audit_mutation_id helper present + preference
    # order (caller → current_mutation_epoch → ResourceQuota → gen).
    must("Issue #2493", "AC1", tma)
    must("resolve_audit_mutation_id", "AC1", tma)
    must("caller_mid != 0", "AC1", tma)
    must("current_mutation_epoch", "AC1", tma)
    must("process_resource_quota_manager().provenance_mutation_id", "AC2", tma)
    must("audit_mid_fallback_gen_total", "AC4", tma)
    must("audit_mid_fallback_gen_total.fetch_add", "AC4", tma)

    # AC3 — AOT hot-update audit uses resolve.
    must("resolve_audit_mutation_id()", "AC3", tma)

    # AC5 — capture_security_correlated_audit updated to use resolve + epoch fallback.
    must("capture_security_correlated_audit", "AC5", tma)
    must("resolve_audit_mutation_id(mutation_id)", "AC5", tma)
    must("epoch != 0 ? epoch : ::aura::core::current_mutation_epoch()", "AC5", tma)

    # AC6 — registrations + ac functions + this linter self-cite.
    must("ac1_prefers_caller_then_mutation_epoch", "AC1", test)
    must("ac2_resource_quota_fallback", "AC2", test)
    must("ac3_aot_hotupdate_uses_resolve", "AC3", test)
    must("ac4_soft_no_activity_fallback", "AC4", test)
    must("ac5_correlated_audit_join", "AC5", test)
    must("ac6_source_and_gate", "AC6", test)
    must("Issue #2493", "AC6", test)
    must("test_audit_mutation_id_unify_2493", "AC6", cmake)
    must("aura_add_issue_test(test_audit_mutation_id_unify_2493)", "AC6", cmake)
    must("aura_issue_test_link_llvm_jit(test_audit_mutation_id_unify_2493)", "AC6", cmake)
    must("check_audit_mutation_id_unify_2493", "AC6", build)
    must("cmd_audit_mutation_id_unify_2493_coverage", "AC6", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2493 audit mutation_id unify — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
