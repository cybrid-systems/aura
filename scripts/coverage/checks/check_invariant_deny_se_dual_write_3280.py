#!/usr/bin/env python3
"""Issue #3280 linter — invariant / boundary deny dual-writes SecurityEvent.

Residual gap (main @ 7d64ca7): cap/isolation + effect allow/deny paths
dual-write into the unified SecurityEvent ring + WAL, but the invariant
suite failure (`record_invariant_audit_result` !all_ok branch) and
MutationBoundary denies (`record_boundary_deny_after_restore` →
`record_boundary_outcome(success=false)`) only stamped the Typed trail
via `capture_audit_event` — no `emit_security_event_durable(InvariantFail)`.
query:security-audit (SE-driven) and query:evolution-audit-decision (last
SE) never saw type/linear/ADT force-rollbacks — the most common self-evo
reject class.

Fix (additive, no second audit bus):
  - shared helper `emit_invariant_deny_se(mid, tenant, fiber, epoch, op,
    deny_kind)` next to join_audit_and_se_mid (#3066 composite/batch pin).
    Production / Full only (Soft/Sampled zero-cost). mid=0 → skip (the
    mid-fallback-refused SE from resolve_audit_mutation_id is the joinable
    refuse evidence). One SE per deny via TLS mid-keyed guard so both
    helpers never double-emit for the same deny.
  - `record_invariant_audit_result` !all_ok branch: after
    capture_audit_event(Error), emit InvariantFail SE with deny_kind
    type/linear/provenance/adt/cross-batch/cross-closure.
  - `record_boundary_deny_after_restore`: after trail stamp (restore
    before stamp per #3217), emit InvariantFail SE with deny_kind
    "boundary".

Gate rows:
  G1  typed_mutation_audit.h cites Issue #3280.
  G2  shared emit_invariant_deny_se helper exists (reuses
      emit_security_event_durable + SecurityEventKind::InvariantFail).
  G3  record_invariant_audit_result !all_ok branch calls the helper.
  G4  record_boundary_deny_after_restore calls the helper.
  G5  one-SE-per-deny TLS mid-keyed guard present.
  G6  Soft/Off/Sampled zero-cost gate (production_defaults || Full).
  G7  mid=0 refuse → no invented InvariantFail (mid-fallback-refused only).
  G8  test ACs in tests/compiler/test_security_audit_unify.cpp
      (#2054 suite home, #81967).
  G9  build.py wires this linter.
  G10 no docs/design/3280-* (per #1655), no tests/issue*/test_issue_3280.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3280 invariant/boundary deny SE dual-write linter ===")
    typed = read("src/compiler/typed_mutation_audit.h")
    test = read("tests/compiler/test_security_audit_unify.cpp")
    build = read("build.py")

    must("#3280" in typed, "G1: typed_mutation_audit.h cites Issue #3280")
    must(
        "emit_invariant_deny_se" in typed
        and "SecurityEventKind::InvariantFail" in typed
        and "emit_security_event_durable" in typed,
        "G2: shared emit_invariant_deny_se helper (SE + InvariantFail)",
    )
    must(
        "record_invariant_audit_result" in typed and "emit_invariant_deny_se" in typed,
        "G3: invariant suite !all_ok branch emits SE",
    )
    must(
        "record_boundary_deny_after_restore" in typed and "emit_invariant_deny_se" in typed,
        "G4: boundary deny-after-restore emits SE",
    )
    must(
        "g_tls_invariant_deny_se_mid" in typed,
        "G5: TLS one-SE-per-deny mid-keyed guard",
    )
    must(
        "production_defaults_active()" in typed and "AuditStrategy::Full" in typed,
        "G6: Soft/Off/Sampled zero-cost gate",
    )
    must(
        "mid == 0" in typed and "mid-fallback-refused" in typed,
        "G7: mid=0 refuse → no invented InvariantFail",
    )
    must("ac3280" in test or "#3280 AC1" in test, "G8a: test AC1 present")
    must("#3280 AC2" in test, "G8b: test AC2 present")
    must("#3280 AC3" in test, "G8c: test AC3 present")
    must("#3280 AC4" in test, "G8d: test AC4 present")
    must("#3280 AC5" in test, "G8e: test AC5 present")
    must(
        "check_invariant_deny_se_dual_write_3280.py" in build,
        "G9: build.py wires linter",
    )
    must(
        not any(p.name.startswith("3280-") for p in (ROOT / "docs/design").glob("3280-*"))
        if (ROOT / "docs/design").exists()
        else True,
        "G10a: no docs/design/3280-* per #1655",
    )
    must(
        not (ROOT / "tests/issues" / "test_issue_3280.cpp").exists()
        and not (ROOT / "tests/compiler" / "test_issue_3280.cpp").exists(),
        "G10b: no tests/issue*/test_issue_3280.cpp per #81967",
    )

    if failures:
        print(f"\n#3280 linter FAILED: {len(failures)} gate(s)")
        return 1
    print("\nOK #3280 invariant_deny_se_dual_write: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
