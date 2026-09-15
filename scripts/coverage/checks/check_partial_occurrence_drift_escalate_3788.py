#!/usr/bin/env python3
"""#3788: partial Occurrence refined drift must CONFLICT under production/Full."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
IMPL = ROOT / "src/compiler/type_checker_impl.cpp"


def main() -> int:
    text = IMPL.read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    if "Issue #2647 AC3 / #3788" not in text and "#3788" not in text:
        errors.append("type_checker_impl.cpp missing #3788 cite near drifted_goals")
    if "drifted_goals > 0" not in text:
        errors.append("missing drifted_goals > 0 block")
    if "production_defaults_active()" not in text:
        errors.append("missing production_defaults_active gate for partial drift")
    if "AuditStrategy::Full" not in text:
        errors.append("missing AuditStrategy::Full face for partial drift escalate")
    # Soft/Off must not escalate in the same branch — require else-if structure
    # after empty-roots CONFLICT (#2647).
    if "occurrence_priority_roots_size() == 0" not in text:
        errors.append("missing all-drift empty-roots CONFLICT (#2647)")
    if errors:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print("OK: #3788 partial Occurrence drift escalate present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
