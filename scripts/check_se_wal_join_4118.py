#!/usr/bin/env python3
"""Issue #4118 source-cite gate: SE WAL / overflow mid join after ring wrap.

query:capability-effect-stats (#3143 replay face) and the #3877 forensic
helpers (forensic_effect_verdict_for_mid / forensic_mid_has_wal_append_miss)
only scanned the in-memory SE ring (1024). After ring_wrap_total advanced,
Agent O(1) mid replay under-reported (se-count=0, verdict None) even when
the SecurityEvent WAL (and the #3109/#3780 overflow ring) still held the
mid — a dual-track residual: query:security-audit already auto-scans WAL +
overflow (#3498/#3674/#3970), the few-query Agent path did not.

ACs:
  AC1  security_event.hh declares the two _full joins (definition lives in
       security_event_wal.hh — that header includes this one, a definition
       here would be circular) and BOTH forensic helpers route a ring miss
       through them behind allow_durable_join; a ring hit never reaches
       the WAL (zero extra I/O on the hot path); cites #4118.
  AC2  security_event_wal.hh defines the joins with the security-audit
       model: wal_mid_lookup_segments() window, then
       find_by_mutation_id_scan_all_segments, then wal_overflow_find_by_mid;
       the #3877/#3879 mutation_wal_append_miss row forces Deny; mid==0
       stays refuse-class ring-only (#3462/#3599); cites #4118.
  AC3  evaluator_primitives_security.cpp joins se-count/se-deny-count
       behind the #3556 production_hard_face_active() gate and passes the
       same gate to both verdict keys — Soft / WAL-off stays ring-only
       with one is_enabled() load max; NO new query:* key, NO new metrics
       bus, NO new insert_kv row (additive keys only at hash END per AC3;
       none added, so the #3339/#3971 hash capacity 186 is untouched);
       cites #4118.
  AC4  tests/core/test_audit_replay_join.cpp extends the existing
       capability-effect-stats / forensic / security-audit family with the
       wrap-past-1024 repro (durable WAL enabled, mid in WAL-only, then
       overflow-only) and is registered in run_test_audit_replay_join;
       no tests/**/test_issue_4118.cpp (per #81934); no docs/design/4118-*;
       build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SE_HH = ROOT / "src" / "core" / "security_event.hh"
SE_WAL_HH = ROOT / "src" / "core" / "security_event_wal.hh"
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_security.cpp"
TST = ROOT / "tests" / "core" / "test_audit_replay_join.cpp"


def _helper_body(text: str, name: str) -> str:
    """Region of a function definition (up to the next [[nodiscard]])."""
    idx = text.find(name)
    if idx < 0:
        return ""
    end = text.find("[[nodiscard]]", idx + len(name))
    return text[idx : end if end > 0 else len(text)]


def main() -> int:
    se = SE_HH.read_text() if SE_HH.exists() else ""
    wal = SE_WAL_HH.read_text() if SE_WAL_HH.exists() else ""
    prim = PRIM.read_text() if PRIM.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1 — declarations + ring-miss routing in security_event.hh.
    decl1 = (
        "forensic_effect_verdict_for_mid_full(std::uint64_t mid) noexcept" in se
        and "forensic_mid_has_wal_append_miss_full(std::uint64_t mid) noexcept" in se
    )
    verdict_body = _helper_body(se, "forensic_effect_verdict_for_mid(\n    std::uint64_t mid, bool allow_durable_join")
    if not verdict_body:
        verdict_body = _helper_body(se, "forensic_effect_verdict_for_mid(std::uint64_t mid, bool allow_durable_join")
    miss_body = _helper_body(se, "forensic_mid_has_wal_append_miss(std::uint64_t mid, bool allow_durable_join")
    routes = (
        verdict_body.count("if (!found && allow_durable_join)") == 1
        and "forensic_effect_verdict_for_mid_full(mid);" in verdict_body
        and miss_body.count("if (!found && allow_durable_join)") == 1
        and "forensic_mid_has_wal_append_miss_full(mid);" in miss_body
    )
    report(
        "AC1",
        "#4118" in se and decl1 and routes,
        "security_event.hh declares the _full joins; both helpers route ring-miss → join behind allow_durable_join; cites #4118",
    )

    # AC2 — security-audit-model join in security_event_wal.hh.
    full1 = _helper_body(wal, "forensic_effect_verdict_for_mid_full(std::uint64_t mid) noexcept {")
    full2 = _helper_body(wal, "forensic_mid_has_wal_append_miss_full(std::uint64_t mid) noexcept {")
    good = (
        "#4118" in wal
        and full1.count("find_recent_by_mutation_id(mid, win)") == 1
        and full1.count("find_by_mutation_id_scan_all_segments(mid)") == 1
        and "wal_overflow_find_by_mid(mid)" in full1
        and "wal_mid_lookup_segments()" in full1
        and "kMutationWalAppendMissReason" in full1
        and full1.count("if (mid == 0)") == 1
        and full2.count("find_by_mutation_id_scan_all_segments(mid)") == 1
        and "wal_overflow_find_by_mid(mid)" in full2
        and "kMutationWalAppendMissReason" in full2
    )
    report(
        "AC2",
        good,
        "security_event_wal.hh defines the join: lookup window → scan-all → overflow; miss row forces Deny; mid==0 ring-only",
    )

    # AC3 — gated primitive join, Soft contract unchanged, no new keys.
    good = (
        "#4118" in prim
        and "se_count == 0 && se_join_durable" in prim
        and "typed_audit::production_hard_face_active()" in prim
        and "find_recent_by_mutation_id(mid, win)" in prim
        and "find_by_mutation_id_scan_all_segments(mid)" in prim
        and "wal_overflow_find_by_mid(mid)" in prim
        # Both verdict keys take the gate (robust to clang-format reflow:
        # count call sites carrying (mid, se_join_durable), not line shape).
        and prim.count("mid, se_join_durable)") >= 2
        # No new query:* key / metrics bus; no new insert_kv row for 4118
        # (additive keys only at hash END — the fix adds none, hash
        # capacity 186 (#3339/#3971) untouched).
        and 'insert_kv("issue-4118"' not in prim
        and 'insert_kv("se-source"' not in prim
        and "query:se-wal-join" not in prim
        and "query:capability-effect-stats" in prim
    )
    report(
        "AC3",
        good,
        "primitive se-count joins WAL/overflow behind production_hard_face_active(); verdict keys take the gate; no new key/bus/capacity",
    )

    # AC4 — test family extension + registration + no invented files.
    no_test_file = not any(p.name == "test_issue_4118.cpp" for p in (ROOT / "tests").rglob("*.cpp"))
    no_doc = not any((ROOT / "docs" / "design").glob("4118-*")) if (ROOT / "docs" / "design").exists() else True
    good = (
        "ac20_se_wal_overflow_mid_join_4118" in tst
        and tst.count("4118 AC1") >= 5
        and tst.count("4118 AC2") >= 3
        and tst.count("4118 AC4") >= 2
        and "4118001" in tst
        and "4118003" in tst
        and "mutation_wal_append_miss" in tst
        and "for (std::uint64_t i = 0; i < 1030; ++i)" in tst
        and "ac20_se_wal_overflow_mid_join_4118();" in tst
        and 'query:security-audit\\" 10 42 0 0 4118001' in tst
        and no_test_file
        and no_doc
        and "check_se_wal_join_4118.py" in build
        and "check_se_wal_join_4118.py" in allow
    )
    report(
        "AC4",
        good,
        "test_audit_replay_join.cpp hosts the wrap repro (WAL + overflow + parity), runner-registered; no test_issue_4118.cpp / docs; build.py + allowlist wired",
    )

    print("check_se_wal_join_4118:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
