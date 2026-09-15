#!/usr/bin/env python3
"""#3794: partial Occurrence drift must refuse type-export / commit until drained."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    impl = (ROOT / "src/compiler/type_checker_impl.cpp").read_text(encoding="utf-8", errors="replace")
    tma = (ROOT / "src/compiler/typed_mutation_audit.h").read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    if "Issue #3794" not in impl:
        errors.append("type_checker_impl.cpp missing #3794 cite")
    if "note_occurrence_partial_drift_grant_refuse" not in impl:
        errors.append("SDO missing note_occurrence_partial_drift_grant_refuse")
    if "clear_occurrence_partial_drift_grant_refuse" not in impl:
        errors.append("SDO missing clear on drained")
    if "kOccurrencePartialDriftGrantRefuseIssue = 3794" not in tma:
        errors.append("missing issue stamp")
    if "occurrence_partial_drift_grant_refuse_face_hit" not in tma:
        errors.append("missing face hit helper")
    if (
        "type_export_residual_faces_clear" in tma
        and "occurrence_partial_drift_grant_refuse_face_hit"
        not in tma[tma.find("type_export_residual_faces_clear") : tma.find("type_export_residual_faces_clear") + 500]
    ):
        errors.append("type_export_residual_faces_clear missing 3794 face")
    if errors:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print("OK: #3794 partial drift grant refuse present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
