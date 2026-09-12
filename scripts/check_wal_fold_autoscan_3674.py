#!/usr/bin/env python3
"""Issue #3674 source-cite gate: evolution-audit-decision fold auto-durables.

The default (no :durable) fold published forensic-source=3 /
durable-hit=0 with an empty reason once the typed (256) + SE (1024)
rings wrapped — the one-query Agent decision surface read "no evidence"
while query:security-audit already auto-scanned the same production
face (#3498/#3603).

ACs:
  AC1: auto_durable arm — production/Full + mid + both rings miss +
       WAL enabled — admitted beside :durable in the scan gate.
  AC2: Soft/Off zero-I/O preserved — the scan block stays gated on
       production/Full; durable-hit key intact.
  AC3: additive faces preserved — typed-trail-miss never rewritten,
       sidecar fill additive (#3242), observe-only + suggested-next
       untouched (#3114/#3246).
  AC4: security-audit path unchanged — bounded wal_mid_lookup_segments()
       window reused, #3498/#3603 fallback shape intact.
  AC5: tests extended — forensic source-cite AC + runtime ACs (no
       new test_issue_N.cpp).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEC = ROOT / "src" / "compiler" / "evaluator_primitives_security.cpp"
FORENSIC = ROOT / "tests" / "compiler" / "test_evolution_audit_decision_forensic.cpp"
RUNTIME = ROOT / "tests" / "core" / "test_audit_replay_join.cpp"

GATE = "if ((want_durable || auto_durable) && join_mid != 0 &&"
ARM = "join_mid != 0 && !typed_hit && !se_ring_has_mid && wal_enabled"
PRODFULL = "production_defaults_active() || get_strategy() == AuditStrategy::Full"


def check_ac1(src: str) -> tuple[bool, str]:
    if "Issue #3674" not in src:
        return False, "source must cite #3674"
    if "const bool auto_durable" not in src:
        return False, "auto_durable arm missing"
    if ARM not in src:
        return False, "auto_durable must require both rings miss + WAL enabled"
    if GATE not in src:
        return False, "scan gate must admit (want_durable || auto_durable)"
    return True, "auto_durable arm gated on rings miss + WAL, admitted beside :durable"


def check_ac2(src: str) -> tuple[bool, str]:
    gate = src.find(GATE)
    if gate < 0:
        return False, "gate missing"
    window = src[gate : gate + 240]
    if PRODFULL not in window:
        return False, "scan block must stay production/Full-gated (Soft: no I/O)"
    if 'insert_kv("durable-hit", durable_hit)' not in src:
        return False, "durable-hit key must stay"
    return True, "Soft/Off zero-WAL-I/O contract preserved"


def check_ac3(src: str) -> tuple[bool, str]:
    if 'insert_kv("typed-trail-miss", typed_miss)' not in src:
        return False, "typed-trail-miss face must stay (WAL is not the typed trail)"
    if "find_recent_typed_summary_by_mid" not in src:
        return False, "additive sidecar fill (#3242) must stay"
    if 'insert_kv("observe-only", 1)' not in src:
        return False, "observe-only face must stay"
    if "decide_evolution_suggested_next" not in src:
        return False, "suggested-next fold must stay (no playbook exec)"
    return True, "additive faces preserved"


def check_ac4(src: str) -> tuple[bool, str]:
    if "wal_mid_lookup_segments()" not in src:
        return False, "bounded lookup window must be reused"
    if "find_recent_by_mutation_id" not in src:
        return False, "WAL mid fallback must stay"
    if "summary_scan_ok" not in src:
        return False, "security-audit summary_scan_ok shape must stay"
    return True, "security-audit path unchanged (bounded window reused)"


def check_ac5() -> tuple[bool, str]:
    fsrc = FORENSIC.read_text() if FORENSIC.exists() else ""
    rsrc = RUNTIME.read_text() if RUNTIME.exists() else ""
    if "#3674" not in fsrc:
        return False, "forensic test must cite #3674 (source-cite AC)"
    if "ac10_wal_fold_autoscan_3674" not in fsrc:
        return False, "forensic test must carry the #3674 AC function"
    if "#3674" not in rsrc:
        return False, "runtime test must cite #3674 (runtime ACs)"
    if "ac11_wal_fold_autoscan_3674" not in rsrc:
        return False, "runtime test must carry the runtime auto-durable AC"
    if "ac12_soft_no_autoscan_3674" not in rsrc:
        return False, "runtime test must carry the Soft no-auto-scan AC"
    return True, "tests extended (forensic + runtime, no new test_issue_N.cpp)"


def main() -> int:
    if not SEC.exists():
        print(f"FAIL: cannot read {SEC}")
        return 1
    src = SEC.read_text()
    ok = True
    for name, fn in (
        ("AC1", lambda: check_ac1(src)),
        ("AC2", lambda: check_ac2(src)),
        ("AC3", lambda: check_ac3(src)),
        ("AC4", lambda: check_ac4(src)),
        ("AC5", check_ac5),
    ):
        good, msg = fn()
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good
    if not ok:
        print("Issue #3674 WAL fold auto-durable linter: FAIL")
        return 1
    print("Issue #3674 WAL fold auto-durable linter: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
