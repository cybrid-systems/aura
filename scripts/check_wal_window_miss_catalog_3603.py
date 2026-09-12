#!/usr/bin/env python3
# scripts/check_wal_window_miss_catalog_3603.py -- Issue #3603 source-cite gate.
#
# Verifies the mid point-query window-miss face + forensic catalog seed
# (#3113/#3338/#3205 residual):
#
#  AC1: query:security-audit additive wal-lookup-window-miss — ring-row and
#       SE-WAL-fallback lines carry miss=0; a synthetic miss line
#       (reason="wal-lookup-window-miss") fires only under
#       production/Full + explicit mid + SE WAL enabled + emitted==0, keeps
#       typed-trail-miss=1, and a typed-summary sidecar hit within the same
#       window keeps the flag 0 (never looks like never-audited).
#  AC2: query:evolution-audit-decision :durable computes
#       wal_lookup_window_miss inside the want_durable + production/Full +
#       WAL-enabled block (all find_recent_* missed) and insert_kv's it;
#       observe-only stays 1.
#  AC3: Soft / WAL-off / no explicit mid stay zero extra I/O (gate rows
#       cite the gating literals).
#  AC4: engine:metrics catalog seed carries the 7 forensic keys;
#       query:dirty-columnar-stats is NOT revived (merged into
#       query:dirty-columnar).
#  AC5: test faces — test_audit_replay_join.cpp drives live wrap+rotate +
#       explicit-mid point-query (set_rotate_bytes + miss-flag asserts);
#       test_engine_metrics_facade.cpp catalog-contains the 7 keys;
#       test_evolution_audit_decision_forensic.cpp source-cites the key.
#  AC6: no docs/design/3603-* (#1655); no tests/**/test_issue_3603.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SEC = "src/compiler/evaluator_primitives_security.cpp"
OBS = "src/compiler/evaluator_primitives_observability.cpp"
JOIN = "tests/core/test_audit_replay_join.cpp"
FACADE = "tests/compiler/test_engine_metrics_facade.cpp"
FORENSIC = "tests/compiler/test_evolution_audit_decision_forensic.cpp"
BUILD = "build.py"

FORENSIC_KEYS: tuple[str, ...] = (
    "query:security-audit",
    "query:security-posture",
    "query:security-schedule-gate",
    "query:reload-recovery-playbook",
    "query:aot-hotupdate-stats",
    "query:aot-incremental-reemit-stats",
    "query:arena-moving-densify-health",
)

REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: additive window-miss face on query:security-audit.
    (SEC, r"Issue\s+#3603", "3603 AC1: security.cpp cites #3603"),
    (
        SEC,
        r'"typed-outcome-wal=\{\} typed-kind-wal=\{\} wal-lookup-window-miss=0"',
        "3603 AC1: ring-row line carries wal-lookup-window-miss=0",
    ),
    (
        SEC,
        r'"typed-outcome-wal=\{\} typed-kind-wal=\{\} wal-lookup-window-miss=0"',
        "3603 AC1: SE-WAL-fallback line carries wal-lookup-window-miss=0",
    ),
    (
        SEC,
        r'reason=\\"wal-lookup-window-miss\\"',
        "3603 AC1: synthetic miss line names the reason",
    ),
    (
        SEC,
        r"typed-trail-miss=1",
        "3603 AC1: miss line keeps typed-trail-miss=1 (not rewritten as hit)",
    ),
    (
        SEC,
        r"wal_mid_lookup_segments\(\)",
        "3603 AC1: miss face uses the bounded lookup window",
    ),
    # AC2: decision hash durable window-miss flag.
    (
        SEC,
        r'insert_kv\("wal-lookup-window-miss", wal_lookup_window_miss\)',
        '3603 AC2: decision hash insert_kv("wal-lookup-window-miss", ...)',
    ),
    (
        SEC,
        r"\(durable_hit == 0 && typed_summary_from_wal == 0\) \? 1 : 0",
        "3603 AC2: flag = all find_recent_* missed semantics",
    ),
    (
        SEC,
        r'insert_kv\("observe-only", 1\)',
        "3603 AC2: observe-only stays 1 (unchanged #3114 face)",
    ),
    # AC4: catalog seed carries the forensic keys.
    *[(OBS, rf'"{re.escape(k)}",', f"3603 AC4: catalog seed has {k}") for k in FORENSIC_KEYS],
    # AC5: test faces.
    (JOIN, r"Issue\s+#3603|#3603", "3603 AC5: replay-join test hosts #3603 ACs"),
    (JOIN, r"set_rotate_bytes\(", "3603 AC5: replay-join drives segment rotation"),
    (JOIN, r"wal_mid_lookup_segments\(\) == 8", "3603 AC5: replay-join pins the prod window"),
    (
        JOIN,
        r"wal-lookup-window-miss=1",
        "3603 AC5: replay-join asserts the miss flag",
    ),
    (
        FACADE,
        r"query:arena-moving-densify-health",
        "3603 AC5: facade catalog-contains the forensic keys",
    ),
    (
        FACADE,
        r"query:dirty-columnar-stats",
        "3603 AC5: facade asserts dirty-columnar-stats stays gone",
    ),
    (
        FORENSIC,
        r"wal-lookup-window-miss",
        "3603 AC5: forensic test source-cites the key",
    ),
    (BUILD, r"check_wal_window_miss_catalog_3603", "3603 AC6: build.py wires the linter"),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    flat = body(SEC)

    # AC1: miss line must sit after the SE-WAL fallback and be gated on
    # production/Full (summary_scan_ok) + SE WAL enabled + explicit mid.
    gate_pos = flat.find("Issue #3603: ring miss + SE WAL find_recent miss")
    if gate_pos == -1:
        failures.append("3603 AC1/AC3: synthetic miss block not found")
    else:
        window = flat[gate_pos : gate_pos + 2200]
        for needle, lbl in (
            ("summary_scan_ok", "3603 AC3: miss block gated by summary_scan_ok (production/Full)"),
            (
                "g_security_event_wal().is_enabled()",
                "3603 AC3: miss block gated by SE WAL enabled",
            ),
            ("filt_mid && want_mid != 0", "3603 AC3: miss block gated by explicit mid"),
            (
                "find_recent_typed_summary_by_mid",
                "3603 AC1: sidecar probe inside the miss block (evidence, not miss)",
            ),
        ):
            if needle not in window:
                failures.append(lbl)

    # AC1: both ring/fallback miss=0 rows + one miss=1 synthetic line.
    count0 = flat.count("wal-lookup-window-miss=0")
    if count0 < 2:
        failures.append(f"3603 AC1: expected >=2 miss=0 line sites, found {count0}")
    if flat.count("wal-lookup-window-miss={}") < 1:
        failures.append("3603 AC1: synthetic miss line formats the flag value")

    # AC2: the flag computation sits inside the durable gate (after
    # "(want_durable || auto_durable) && join_mid != 0" and before
    # insert_kv of the key). #3674: the gate admits the production/Full
    # auto-durable arm beside the explicit :durable force — same block,
    # same window-miss computation inside it.
    # Match the computation (not the "= 0" declaration, which precedes
    # the gate).
    gate2 = flat.find("if ((want_durable || auto_durable) && join_mid != 0 &&")
    comp = flat.find("(durable_hit == 0 && typed_summary_from_wal == 0) ? 1 : 0")
    ins = flat.find('insert_kv("wal-lookup-window-miss"')
    if gate2 == -1 or comp == -1 or ins == -1:
        failures.append("3603 AC2: durable gate / computation / insert anchors missing")
    elif not (gate2 < comp < ins):
        failures.append("3603 AC2: window-miss must be computed inside the durable block")

    # AC3: the miss face never fires under Soft — the fallback-style gate
    # reuses summary_scan_ok; assert the ordering once more at the key.
    if gate_pos != -1 and flat.find("const bool summary_scan_ok =") > gate_pos:
        failures.append("3603 AC3: summary_scan_ok must be defined before the miss block")

    # AC4: the merged-away name must not be resurrected anywhere in src/.
    for p in (REPO_ROOT / "src" / "compiler").glob("*.cpp"):
        if "query:dirty-columnar-stats" in read(p):
            failures.append(f"3603 AC4: query:dirty-columnar-stats revived in {p.name}")
    for p in (REPO_ROOT / "src" / "compiler").glob("*.ixx"):
        if "query:dirty-columnar-stats" in read(p):
            failures.append(f"3603 AC4: query:dirty-columnar-stats revived in {p.name}")

    # AC6: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3603-"):
                failures.append(f"3603 AC6: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3603.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3603.cpp",
    ):
        if probe.exists():
            failures.append(f"3603 AC6: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3603 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any missing row")
    ap.parse_args()
    failures = run_checks()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"check_wal_window_miss_catalog_3603: {len(failures)} failure(s)")
        return 1
    print("check_wal_window_miss_catalog_3603: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
