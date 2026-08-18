#!/usr/bin/env python3
"""Issue #3114: query:evolution-audit-decision observe-only fold.

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

Extend test_security_audit_unify + test_engine_metrics_facade.
No test_issue_3114.cpp. No docs/design/3114-* per #1655.

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
