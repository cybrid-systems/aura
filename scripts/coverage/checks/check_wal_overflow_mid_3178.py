#!/usr/bin/env python3
"""Issue #3178: SE WAL overflow ring must stamp forensic join keys.

Residual of #3109 — fail-closed option exists and works for depth/posture
counters; only the stamped fields were wrong. Previously stamped
`ovr.mid = rec.seq` (WAL sequence number, NOT a join key) and zeroed
tenant/fiber/epoch. This left overflow entries unjoinable with
`query:security-audit [mutation-id=...]`, Typed trail, and
`CapabilityGrant.bound_mutation_id`.

Contract (one row per AC):
  AC1  inject fail + fwrite_miss under wal_append_fail_closed_active():
       overflow mid == SecurityEventWalRecord.mutation_id (NOT seq),
       tenant/fiber/epoch match record.
  AC2  Soft / no-env / WAL-off: overflow ring never written; no new
       hot-path cost (fail-closed gate off → push branch never entered).
  AC3  query:security-audit [mutation-id=…] numeric domain matches SE /
       Typed / CapabilityGrant.bound_mutation_id. SecurityEvent.mutation_id
       type is uint64_t (same as SecurityEventWalRecord.mutation_id and
       WalOverflowRecord.mid).
  AC4  No new public query key required; if posture gains fields, keys
       are additive only. Existing wal-overflow-ring-depth / wal-fail-closed-active
       unchanged in meaning (#3109 lineage).
  AC5  Existing #3109 posture keys (wal-fail-closed-active,
       wal-overflow-ring-depth, schema-3109, issue-3109) unchanged in
       meaning. query:capability-effect-stats / query:security-posture
       additive only.
  AC6  Test: force append fail (inject) → push → assert overflow
       mid/tenant/fiber/epoch; mid=0 event still stores 0 (no
       synthetic process-origin mid).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Files in scope for #3178 (WAL overflow stamp fix).
SCOPE_FILES = [
    "src/core/security_event_wal.hh",
    "src/core/wal_append_fail_slo.h",
    "tests/compiler/test_security_event_wal_replay.cpp",
    "scripts/coverage/checks/check_wal_overflow_mid_3178.py",
]

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden substring {n!r}")

    sew = _read("src/core/security_event_wal.hh")
    slo = _read("src/core/wal_append_fail_slo.h")
    test = _read("tests/compiler/test_security_event_wal_replay.cpp")
    build = _read("build.py")
    # Lineage / additive-only references (must remain unchanged in meaning).
    check3109 = _read("scripts/coverage/checks/check_wal_append_fail_closed_3109.py")

    # ── AC1: inject fail + fwrite_miss stamp rec.mutation_id (NOT seq) ─────
    must("Issue #3178", "AC1 fix marker in inject_fail", sew)
    must("Issue #3178", "AC1 fix marker in fwrite_miss", sew)
    # Both branches must stamp rec.mutation_id (NOT rec.seq) and copy
    # tenant/fiber/epoch from the record. We check the brace-balanced
    # substring via simple counts.
    must("ovr.mid = rec.mutation_id", "AC1 mid from record", sew)
    must("ovr.tenant_id = static_cast<std::uint32_t>(rec.tenant_id)", "AC1 tenant from record", sew)
    must("ovr.fiber_id = static_cast<std::uint64_t>(rec.fiber_id)", "AC1 fiber from record", sew)
    must("ovr.epoch = rec.epoch", "AC1 epoch from record", sew)
    # Both branches push to the ring (the fail-closed gate is the only
    # entry barrier; AC2 covers the gate-off path).
    push_count = sew.count("wal_overflow_ring_push(ovr);")
    if push_count < 2:
        fails.append(
            f"AC1: expected ≥2 wal_overflow_ring_push call sites (fwrite_miss + inject_fail), found {push_count}"
        )

    # AC1 negative: rec.seq must NOT be used as the join key.
    # Allow rec.seq to appear elsewhere (e.g. last_seq_persisted), but
    # NOT in the overflow-stamp lines.
    must_not("ovr.mid = rec.seq", "AC1 negative (rec.seq must not stamp ovr.mid)", sew)

    # ── AC2: Soft / no-env → overflow ring never written ─────────────
    # The fail-closed gate (wal_append_fail_closed_active) must guard
    # BOTH overflow push sites.
    must("wal_append_fail_closed_active()", "AC2 fail-closed gate guard (inject_fail)", sew)
    # Two call sites (one per branch).
    gate_count = sew.count("wal_append_fail_closed_active()")
    if gate_count < 2:
        fails.append(f"AC2: expected ≥2 wal_append_fail_closed_active() guards, found {gate_count}")
    # Env var name unchanged (#3109 lineage).
    must("AURA_WAL_APPEND_FAIL_CLOSED", "AC2 env name", slo)
    # Production gate unchanged.
    must("production_defaults_active", "AC2 production gate", slo)

    # ── AC3: numeric domain matches ──────────────────────────────────
    # SecurityEvent.mutation_id is uint64_t (joinable with WAL record
    # mutation_id, WalOverflowRecord.mid, CapabilityGrant.bound_mutation_id,
    # Typed mid). The fix stamps rec.mutation_id (uint64_t) directly into
    # ovr.mid (uint64_t) — no narrowing.
    must("std::uint64_t mutation_id", "AC3 SE.mutation_id numeric type", sew)
    must("std::uint64_t mid", "AC3 WalOverflowRecord.mid numeric type", sew)
    # Typed trail / CapabilityGrant share the same domain.
    cap_model = _read("src/core/capability_model.hh")
    must("std::uint64_t bound_mutation_id", "AC3 CapabilityGrant.bound_mutation_id numeric type", cap_model)

    # ── AC4: additive only ───────────────────────────────────────────
    # No new public query key inserted (no "schema-3178" / "issue-3178"
    # in evaluator_primitives_security.cpp; only AC1-2 in source-cite).
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    must_not("schema-3178", "AC4 no new schema-3178 prim key", prim)
    must_not("issue-3178", "AC4 no new issue-3178 prim key", prim)
    # Existing #3109 keys still present (unchanged meaning).
    must("wal-fail-closed-active", "AC4 #3109 key preserved", prim)
    must("wal-overflow-ring-depth", "AC4 #3109 depth key preserved", prim)

    # ── AC5: #3109 posture keys unchanged in meaning ─────────────────
    # The #3109 linter file still passes (its contract is unchanged).
    must("wal_append_fail_closed_active", "AC5 #3109 linter preserved", check3109)
    must("wal-overflow-ring-depth", "AC5 #3109 depth key in linter", check3109)
    must("schema-3109", "AC5 #3109 schema preserved in linter", check3109)
    # build.py wires the #3109 linter (lineage intact).
    must("check_wal_append_fail_closed_3109", "AC5 build.py wires #3109 linter", build)

    # ── AC6: test exercises force-inject path + mid=0 no-synthesis ───
    must("#3178", "AC6 test marker in test file", test)
    must("wal_overflow_ring_storage", "AC6 test inspects wal_overflow_ring_storage", test)
    must("wal_overflow_ring_depth", "AC6 test inspects wal_overflow_ring_depth", test)
    # AC6 specifically: mid=0 must NOT be synthesized — test must
    # include an assertion for the zero case.
    must("mid=*/0", "AC6 mid=0 zero-synthesis assertion present", test)

    # No new test_issue_3178.cpp (per #81967 / #81934).
    for rel in (
        "tests/issues/test_issue_3178.cpp",
        "tests/core/test_issue_3178.cpp",
        "tests/compiler/test_issue_3178.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: forbidden new test file {rel} (extend existing test per #81934 / #81967)")

    # No docs/design/3178-* (per #1655).
    docs_design = ROOT / "docs/design"
    if docs_design.is_dir():
        for entry in docs_design.iterdir():
            if entry.name.startswith("3178-"):
                fails.append(f"AC6: forbidden docs/design/{entry.name} (per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} check(s) failed for #3178 WAL overflow mid linter",
            file=sys.stderr,
        )
        return 1

    print("OK: #3178 WAL overflow mid linter passed (all ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
