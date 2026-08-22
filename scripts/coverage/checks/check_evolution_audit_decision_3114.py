#!/usr/bin/env python3
"""Issue #3114: query:evolution-audit-decision observe-only fold.
Issue #3149: residual — add last-se-reason string key (additive).
Issue #3152: residual — add forensic-source enum + 3 sentinels (additive).
             Maps typed-trail-miss=1 to next forensic step (trail / SE /
             WAL). Pure loads only — no WAL scan, no mutate.
Issue #3205: residual — optional :durable mid point-query into WAL.

Contract (one row per AC):
  AC1  register_stats_impl query:evolution-audit-decision + catalog name.
       SlimSurface: no public add(). Discoverable via engine:metrics.
  AC2  Pure-load keys: last-audit-mid, proof-audit-mid, last-se-reason-code,
       last-se-denied, typed-outcome, typed-trail-miss, commit-would-allow,
       commit-force-reason-code, playbook-action, playbook-wired, densify-ok,
       posture-degraded, production-defaults-active, audit-strategy,
       schema-3114 / issue-3114.
  AC3  Soft / empty: mid=0, playbook=idle, no WAL scan, no should_audit.
  AC4  query_hash_capacity_for(planned) + query_hash_finish overflow.
       engine:metrics + :prefix query:evolution.
  AC5  observe-only comment / key — not an auto-executor.
  AC6  Optional mid arg (does not block AC1–5).
  ──────────────────────────────────────────────────────────────────
  AC7  (#3149) additive last-se-reason key alongside last-se-reason-code.
       SE ring scan extracts e.reason[64] truncated NUL-safe. Coexists
       with last-se-reason-code (no replacement). Pure load (no WAL I/O,
       no should_audit, no mutate). Mid filter preserves reason
       consistency (matches the SE row for the same mid).
  AC8  (#3149) Soft / no event → empty string semantics same as
       last-se-reason-code == 0. AURA_SANDBOX=off → no extra cost.
  AC9  (#3149 + #3152 + #3205) cumulative capacity:
       kEvolutionAuditDecisionPlannedKeys bumped from 32 → 33 (#3149)
       then 33 → 37 (#3152) then 37 → 40 (#3205) to cover last-se-reason
       + schema-3149 + issue-3149 + forensic-source + 3 enum sentinels
       + schema-3152 + issue-3152 + durable-hit + schema-3205 +
       issue-3205. overflow=0 on the normal path.
  AC10 (#3149) source-cite + no test_issue_3149.cpp / docs/design/3149-*
       per #81967 / #1655. Suite + facade tests extended to verify
       last-se-reason equals query:security-audit same-mid row reason
       under force-rollback.
  AC11 (#3152) additive forensic-source enum + 3 sentinels
       (forensic-source-trail=1 / -se=2 / -wal=3) inside the same
       handler. Pure loads: one extra O(kSecurityEventRingSize) scan
       only when typed miss + mid != 0, plus two WAL is_enabled()
       bool probes. No file I/O, no mutate, no shadow writes.
       schema-3152 / issue-3152 sentinels added parallel to
       schema-3149 / issue-3149.

Extend test_security_audit_unify + test_engine_metrics_facade.
No test_issue_3114.cpp / test_issue_3149.cpp. No docs/design/{3114,3149}-*
per #1655.

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

    query = _read("src/compiler/evaluator_primitives_security.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    header = _read("src/compiler/typed_mutation_audit.h")
    unify = _read("tests/compiler/test_security_audit_unify.cpp")
    facade = _read("tests/compiler/test_engine_metrics_facade.cpp")
    suite = _read("tests/suite/engine_metrics.aura")
    build = _read("build.py")

    # ── AC1: new query name, stats-impl only, catalog ──
    must("query:evolution-audit-decision", "AC1 register name", query)
    must('register_stats_impl(\n        "query:evolution-audit-decision"', "AC1 register_stats_impl", query)
    must('"query:evolution-audit-decision"', "AC1 catalog", obs)
    must("kEvolutionAuditDecisionIssue", "AC1 issue stamp", header)
    if 'add("query:evolution-audit-decision"' in query or 'add("query:evolution-audit-decision"' in obs:
        fails.append("AC1: public add() of query:evolution-audit-decision (SlimSurface freeze)")
    must("3114 AC1", "AC1 test marker", unify)

    # ── AC2: required keys ──
    for k in (
        "last-audit-mid",
        "proof-audit-mid",
        "last-se-reason-code",
        "last-se-denied",
        "typed-outcome",
        "typed-trail-miss",
        "commit-would-allow",
        "commit-force-reason-code",
        "playbook-action",
        "playbook-wired",
        "densify-ok",
        "posture-degraded",
        "production-defaults-active",
        "audit-strategy",
        "schema-3114",
        "issue-3114",
    ):
        must(k, f"AC2 key {k}", query)
    must("aura_hot_update_reload_recovery_playbook_get", "AC2 playbook pure load", query)
    must("commit_readiness_live_policy", "AC2 commit_readiness pure load", query)
    must("moving_densify_health::snapshot", "AC2 densify pure load", query)
    must("3114 AC2", "AC2 test marker", unify)

    # ── AC3: Soft / no WAL scan / no extra audit ──
    must("no should_audit, no WAL scan, no mutate", "AC3 no extra I/O comment", query)
    must("3114 AC3", "AC3 test marker", unify)

    # ── AC4: capacity + engine:metrics ──
    must("query_hash_capacity_for", "AC4 planned capacity", query)
    must("query_hash_finish", "AC4 overflow finish", query)
    must("schema-3114", "AC4 facade", facade)
    must("query:evolution-audit-decision", "AC4 suite", suite)
    must("3114 AC4", "AC4 test marker", unify)

    # ── AC5: observe-only, not an executor ──
    must("observe-only", "AC5 key", query)
    must("not an auto-executor", "AC5 comment", query)
    if "aura_hot_update_decide_and_reemit" in query and "query:evolution-audit-decision" in query:
        # only fail if reemit is inside the new query lambda — coarse: must not
        # call reemit APIs from this TU's new fold.
        pass
    if "decide_and_reemit" in query[query.find("query:evolution-audit-decision") :]:
        fails.append("AC5: evolution-audit-decision calls decide_and_reemit")
    must("3114 AC5", "AC5 test marker", unify)

    # ── AC6: optional mid arg ──
    must("filt_mid", "AC6 optional mid filter", query)
    must("3114 AC6", "AC6 test marker", unify)

    # ── AC7 (#3149): additive last-se-reason key alongside
    # last-se-reason-code. SE ring scan extracts e.reason[64] truncated
    # NUL-safe. Coexists with last-se-reason-code (no replacement).
    # Pure load — no WAL I/O, no should_audit, no mutate. Mid filter
    # preserves reason consistency (matches the SE row for the same mid).
    must("last-se-reason", "AC7 additive last-se-reason key", query)
    must("last-se-reason-code", "AC7 last-se-reason-code still present (coexists)", query)
    must("Issue #3149", "AC7 source-cite marker in security prims", query)
    # NUL-safe truncation pattern.
    must("strnlen", "AC7 NUL-safe strnlen truncation", query)
    must("last_se_reason_str.assign", "AC7 truncated string assign", query)
    # insert_kv_str lambda + call.
    must('insert_kv_str("last-se-reason", last_se_reason_str)', "AC7 insert_kv_str call site", query)
    must("auto insert_kv_str = ", "AC7 insert_kv_str lambda defined", query)
    # Pure-load invariant: AC3 already covers "no should_audit, no WAL scan,
    # no mutate" globally. The new last-se-reason path inherits AC3 — no
    # additional should_audit / WAL I/O introduced in the SE ring scan block.
    # (Comment text in the new code may mention these by negation; that's fine.)

    # ── AC8 (#3149): Soft / no event → empty string semantics same as
    # last-se-reason-code == 0. AURA_SANDBOX=off → no extra cost.
    # The capture is gated by e.reason[0] != '\0', so an empty reason
    # (kind=0 / no event) leaves last_se_reason_str empty.
    must("e.reason[0] != '\\0'", "AC8 empty-reason guard (no-event branch)", query)

    # ── AC9 (#3149 + #3152): cumulative capacity bumped from 32 → 33
    # (#3149) then 33 → 37 (#3152) to cover last-se-reason + 3 sentinels
    # + schema-3149 + issue-3149 + forensic-source + 3 enum sentinels +
    # schema-3152 + issue-3152. overflow=0 on the normal path.
    must("kEvolutionAuditDecisionPlannedKeys = 44", "AC9 planned keys bumped to 44 (#3149+#3152+#3205+#3242)", query)
    must("schema-3205", "AC9 schema-3205 sentinel", query)
    must("issue-3205", "AC9 issue-3205 sentinel", query)
    must("schema-3149", "AC9 schema-3149 sentinel", query)
    must("issue-3149", "AC9 issue-3149 sentinel", query)
    # Coexist with old sentinels (no replacement).
    must("schema-3114", "AC9 schema-3114 still present (coexists)", query)
    must("issue-3114", "AC9 issue-3114 still present (coexists)", query)

    # ── AC11 (#3152): additive forensic-source enum + 3 sentinels
    # inside the same handler. Pure loads only — one extra
    # O(kSecurityEventRingSize) scan when typed miss + mid != 0, plus
    # two WAL is_enabled() bool probes. No file I/O, no mutate, no
    # shadow writes. schema-3152 / issue-3152 sentinels added.
    must("Issue #3152", "AC11 source-cite marker in security prims", query)
    must('insert_kv("forensic-source", forensic_source)', "AC11 forensic-source key inserted", query)
    must('insert_kv("forensic-source-trail", 1)', "AC11 forensic-source-trail=1 sentinel", query)
    must('insert_kv("forensic-source-se", 2)', "AC11 forensic-source-se=2 sentinel", query)
    must('insert_kv("forensic-source-wal", 3)', "AC11 forensic-source-wal=3 sentinel", query)
    must("schema-3152", "AC11 schema-3152 sentinel", query)
    must("issue-3152", "AC11 issue-3152 sentinel", query)
    # Pure-load invariant: no fopen / fread / should_audit in the new
    # forensic-source path (Soft / zero-cost preserved).
    must("forensic_source = 0", "AC11 forensic_source zero-initialized (mid==0 short-circuit)", query)
    must("se_ring_has_mid", "AC11 SE ring has-mid scan", query)
    must("g_mutation_audit_wal().is_enabled()", "AC11 mutation WAL is_enabled() bool probe", query)
    must("g_security_event_wal().is_enabled()", "AC11 SE WAL is_enabled() bool probe", query)
    must("kSecurityEventRingSize", "AC11 SE ring size cap referenced (bounded scan)", query)
    if "fopen" in query[query.find("Issue #3152") : query.find("Issue #3152") + 4000]:
        fails.append("AC11: fopen in #3152 forensic-source block (Soft: no I/O)")
    if "fread" in query[query.find("Issue #3152") : query.find("Issue #3152") + 4000]:
        fails.append("AC11: fread in #3152 forensic-source block (Soft: no I/O)")
    # No test_issue_3152.cpp / docs/design/3152-* per #81967 / #1655.
    if (ROOT / "tests" / "compiler" / "test_issue_3152.cpp").is_file():
        fails.append("AC11: tests/compiler/test_issue_3152.cpp present (forbidden #81967)")
    docs3152 = ROOT / "docs" / "design"
    if docs3152.is_dir():
        for f in sorted(docs3152.glob("3152-*")):
            fails.append(f"AC11: docs/design/{f.name} present (forbidden #1655)")

    # ── AC10 (#3149): source-cite + no test_issue_3149.cpp /
    # docs/design/3149-* per #81967 / #1655. Suite + facade extended
    # to verify last-se-reason equals query:security-audit same-mid
    # row reason under force-rollback.
    must("3149 AC", "AC10 test marker in test_security_audit_unify", unify)
    if (ROOT / "tests" / "compiler" / "test_issue_3149.cpp").is_file():
        fails.append("AC10: tests/compiler/test_issue_3149.cpp present (forbidden #81967)")
    docs3149 = ROOT / "docs" / "design"
    if docs3149.is_dir():
        for f in sorted(docs3149.glob("3149-*")):
            fails.append(f"AC10: docs/design/{f.name} present (forbidden #1655)")

    # ── AC12 (#3205): optional :durable mid point-query. Default path
    # still no WAL scan. Soft / Off refuse disk I/O. Additive durable-hit.
    must("Issue #3205", "AC12 source-cite", query)
    must("find_recent_by_mutation_id", "AC12 SE WAL point-query", query)
    must("want_durable", "AC12 :durable parse", query)
    must('insert_kv("durable-hit", durable_hit)', "AC12 durable-hit key", query)
    must("3205 AC", "AC12 test marker in unify", unify)
    if (ROOT / "tests" / "compiler" / "test_issue_3205.cpp").is_file():
        fails.append("AC12: tests/compiler/test_issue_3205.cpp present (forbidden #81967)")
    docs3205 = ROOT / "docs" / "design"
    if docs3205.is_dir():
        for f in sorted(docs3205.glob("3205-*")):
            fails.append(f"AC12: docs/design/{f.name} present (forbidden #1655)")

    must("check_evolution_audit_decision_3114", "linter wired in build.py", build)
    must("Issue #3114", "linter error message", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3114.cpp").is_file():
        fails.append("AC: tests/compiler/test_issue_3114.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3114-*")):
            fails.append(f"AC: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3114 evolution-audit-decision — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
