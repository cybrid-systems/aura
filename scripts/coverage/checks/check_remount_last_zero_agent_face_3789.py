#!/usr/bin/env python3
"""#3789: remount last==0 strip must Agent-deny (not Quiet-as-ok)."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    tma = (ROOT / "src/compiler/typed_mutation_audit.h").read_text(encoding="utf-8", errors="replace")
    hh = (ROOT / "src/compiler/type_linear_commit_health.hh").read_text(encoding="utf-8", errors="replace")
    rf = (ROOT / "src/compiler/evaluator_primitives_query_reflect.cpp").read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    if "g_remount_last_zero_strip_face" not in tma:
        errors.append("missing remount_last_zero_strip_face latch")
    if "Issue #3789" not in tma:
        errors.append("typed_mutation_audit.h missing #3789 cite")
    if "remount_last_zero_strip" not in hh:
        errors.append("evolution snapshot missing remount_last_zero_strip")
    if "kRemountLastZeroForceReasonCode" not in tma:
        errors.append("missing force_reason code 17")
    if "remount-last-zero-strip" not in rf:
        errors.append("query missing remount-last-zero-strip key")
    if "schema-3789" not in rf:
        errors.append("query missing schema-3789")
    # Quiet outcome retained (do not Require Reject)
    if "kTypeLinearProofOutcomeQuiet" not in tma:
        errors.append("Quiet outcome path missing")
    if errors:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print("OK: #3789 remount last==0 Agent face present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
