#!/usr/bin/env python3
"""Issue #3338: long-run WAL mid lookup window + optional retention.

Production force_wal segments grow unbounded; find_recent default
max_segments=2 misses older durable mids. Raise production lookup
window (env AURA_WAL_MID_LOOKUP_SEGMENTS, default 8) and optional
AURA_WAL_MAX_SEGMENTS prune. Soft / WAL-off: no scan, no prune.

Contract (one row per AC):
  AC1  WAL-off / Soft: no prune on quiet path; lookup default 2
  AC2  production lookup helper default 8; durable path uses it
  AC3  rotate prune when AURA_WAL_MAX_SEGMENTS set; unset = 0
  AC4  explicit max_segments=2 retained; #3109 overflow keys kept
  AC5  tests in test_security_event_wal_replay + test_security_audit_unify;
       linter after #3205; no invent / no docs/design

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

    slo = _read("src/core/wal_append_fail_slo.h")
    se_wal = _read("src/core/security_event_wal.hh")
    mut_wal = _read("src/core/mutation_audit_wal.hh")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    replay = _read("tests/compiler/test_security_event_wal_replay.cpp")
    unify = _read("tests/compiler/test_security_audit_unify.cpp")
    lint3205 = _read("scripts/coverage/checks/check_evolution_audit_decision_durable_3205.py")
    lint3109 = _read("scripts/coverage/checks/check_wal_append_fail_closed_3109.py")
    build = _read("build.py")

    must("kWalMidLookupWindowIssue = 3338", "AC1 stamp", slo)
    must("wal_mid_lookup_segments", "AC1 helper", slo)
    must("kWalMidLookupSegmentsSoft = 2", "AC1 Soft default 2", slo)
    must("AURA_WAL_MID_LOOKUP_SEGMENTS", "AC1 env", slo)
    must("3338 AC1", "AC1 test", replay)

    must("kWalMidLookupSegmentsProduction = 8", "AC2 production default 8", slo)
    must("wal_mid_lookup_segments()", "AC2 durable SE lookup", sec)
    must("wal-mid-lookup-segments", "AC2 posture key", sec)
    must("wal-mid-lookup-segments", "AC2 obs posture key", obs)
    must("3338 AC2", "AC2 test", replay)

    must("AURA_WAL_MAX_SEGMENTS", "AC3 retention env", slo)
    must("wal_max_segments_retention", "AC3 helper", slo)
    must("prune_old_segments_unlocked", "AC3 SE prune", se_wal)
    must("prune_old_segments_unlocked", "AC3 mutation prune", mut_wal)
    must("security_event_wal_segment_prune_total{0}", "AC3 SE counter END", se_wal)
    must("audit_wal_segment_prune_total{0}", "AC3 mutation counter END", mut_wal)
    must("wal-max-segments-retention", "AC3 query key", sec)
    must("wal-segment-prune-total", "AC3 prune key", sec)
    must("3338 AC3", "AC3 test", replay)
    se_pr = se_wal.find("security_event_wal_segments{0}")
    se_pn = se_wal.find("security_event_wal_segment_prune_total{0}")
    if se_pn < 0 or se_pr < 0 or se_pn < se_pr:
        fails.append("AC3: SE prune counter must END-append after segments")

    must("max_segments = 2", "AC4 default 2", se_wal)
    must("wal-overflow-ring-depth", "AC4 #3109 key", sec)
    must("find_recent_by_mutation_id", "AC4 #3205 API", se_wal)
    must("3338 AC4", "AC4 test", replay)

    must("check_wal_mid_lookup_window_3338", "AC5 build.py", build)
    must("check_evolution_audit_decision_durable_3205", "AC5 #3205 wired", build)
    must("Issue #3205", "AC5 #3205 linter retained", lint3205)
    must("Issue #3109", "AC5 #3109 linter retained", lint3109)
    must("ac3338_5_no_invent", "AC5 test", replay)
    must("schema-3338", "AC5 unify", unify)
    prev = build.find("check_evolution_audit_decision_durable_3205")
    ours = build.find("check_wal_mid_lookup_window_3338")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3205")
    if (ROOT / "tests" / "compiler" / "test_issue_3338.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3338.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3338-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3338 WAL mid lookup window — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
