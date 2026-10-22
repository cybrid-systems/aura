#!/usr/bin/env python3
"""Issue #4657: wal_append_fail_slo.h header matches the fail-closed table.

The header's top block claimed a blanket fail-open residual while
decide_wal_append_fail_slo / evaluate_wal_append_fail_slo are
fail-closed on the force_wal face: a force_wal enable/append miss denies
the SAME mutate (#3639 effect gate) and the next outermost mutate
(#3211 schedule gate); default WAL is observe-only with the explicit
AURA_WAL_APPEND_FAIL_OPEN opt-out as the only fail-open face; Soft/Off
stays zero-cost. Doc-only contract:

  AC1  every "fail-open" mention in the header sits on the explicit
       AURA_WAL_APPEND_FAIL_OPEN opt-out line (no blanket claim)
  AC2  the contract sentence names all three faces and the old
       "fail-open residual" wording is gone
  AC3  the decision-table comment still pins the force_wal arm row and
       the Soft observe-only row (contract anchor)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADER = "src/core/wal_append_fail_slo.h"


def main() -> int:
    fails: list[str] = []
    path = ROOT / HEADER
    if not path.is_file():
        print(f"FAIL: missing {HEADER}", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # ── AC1: fail-open only on the explicit opt-out line ────────────────
    fail_open_lines = [ln for ln in lines if "fail-open" in ln]
    if not fail_open_lines:
        fails.append(
            "AC1: no fail-open mention at all — the explicit AURA_WAL_APPEND_FAIL_OPEN opt-out must stay documented"
        )
    for ln in fail_open_lines:
        if "AURA_WAL_APPEND_FAIL_OPEN" not in ln:
            fails.append(f"AC1: blanket fail-open claim (no AURA_WAL_APPEND_FAIL_OPEN on line): {ln.strip()!r}")

    # ── AC2: contract sentence present, old blanket wording gone ────────
    for needle in (
        "WAL append-fail decision table is fail-closed on the force_wal face",
        "fail-open via explicit AURA_WAL_APPEND_FAIL_OPEN only",
        "force_wal = enable/append miss fail-closed",
        "Soft / Off = zero-cost",
    ):
        if needle not in text:
            fails.append(f"AC2: missing contract sentence fragment {needle!r}")
    if "fail-open residual" in text:
        fails.append("AC2: stale blanket 'fail-open residual' claim still present")

    # ── AC3: decision-table anchor rows survive ─────────────────────────
    idx = text.find("Pure decision table")
    if idx < 0:
        fails.append("AC3: decision-table comment anchor missing")
    else:
        window = text[idx : idx + 400]
        if "wal-enable-failed (arm)" not in window:
            fails.append("AC3: force_wal arm row missing from decision table")
        if "observe only (never arm)" not in window:
            fails.append("AC3: Soft observe-only row missing from decision table")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4657 wal SLO header contract — fail-closed table, fail-open only via AURA_WAL_APPEND_FAIL_OPEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
