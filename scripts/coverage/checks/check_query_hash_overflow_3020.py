#!/usr/bin/env python3
"""Issue #3020: domain query:* hash builders fail-soft on insert miss.

Contract (one row per AC):
  AC1  Inventory: high-churn builders size from planned*2 (or ≥64) via
       query_hash_capacity_for / insert_kv_checked. Residual create(N)
       catalogs documented as headroom-3020 (static key count << 0.7*N
       or already N≥64).
  AC2  Insert miss stamps overflow=1 and bumps query_hash_overflow_total;
       never silently drop. Forced-full test in test_engine_metrics_facade.
  AC3  query:reload-recovery-playbook / query:security-posture /
       query:type-linear-commit-health keep documented schema sentinels.
  AC4  Extend test_engine_metrics_facade (+ engine_metrics.aura). Soft/Off
       extra cost is one force-cap load; no second metrics bus.
  AC5  Soak: no query_hash_overflow_total bumps under default catalog.
       No test_issue_3020.cpp; no docs/design/ (#1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _slice_after(hay: str, needle: str, n: int = 20000) -> str:
    i = hay.find(needle)
    if i < 0:
        return ""
    return hay[i : i + n]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator.ixx")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ref = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    jit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_engine_metrics_facade.cpp")
    suite = _read("tests/suite/engine_metrics.aura")
    build = _read("build.py")
    fields = _read("src/compiler/compiler_metrics_fields.inc")

    # ── AC1: shared helper + inventory ──
    must("Issue #3020", "AC1", ev)
    must("insert_kv_checked", "AC1 helper", ev)
    must("query_hash_capacity_for", "AC1 size", ev)
    must("planned_keys) * 2", "AC1 headroom planned*2", ev)
    must("headroom-3020", "AC1 residual inventory", ev)
    must("security-posture", "AC1 inventory high-churn", ev)
    must("type-linear-commit-health", "AC1 inventory high-churn", ev)
    must("reload-recovery-playbook", "AC1 inventory high-churn", ev)

    sec_q = _slice_after(sec, '"query:security-posture"')
    must("query_hash_capacity_for", "AC1 security-posture size", sec_q)
    must("insert_kv_checked", "AC1 security-posture insert", sec_q)
    must("query_hash_finish", "AC1 security-posture finish", sec_q)

    pb_q = _slice_after(mut, '"query:reload-recovery-playbook"')
    must("query_hash_capacity_for", "AC1 playbook size", pb_q)
    must("insert_kv_checked", "AC1 playbook insert", pb_q)
    must("query_hash_finish", "AC1 playbook finish", pb_q)

    tl_q = _slice_after(ref, '"query:type-linear-commit-health",')
    must("query_hash_capacity_for", "AC1 type-linear size", tl_q)
    must("insert_kv_checked", "AC1 type-linear insert", tl_q)
    must("query_hash_finish", "AC1 type-linear finish", tl_q)

    # ── AC2: fail-soft overflow ──
    must("query_hash_stamp_overflow", "AC2 stamp", ev)
    must('"overflow"', "AC2 overflow key", ev)
    must("g_query_hash_overflow_total", "AC2 counter", ev)
    must("query_hash_overflow_total", "AC2 process counter name", ev)
    must("#3020 AC2: overflow=1 visible when capacity is artificially low", "AC2 test", test)
    must("aura_query_hash_set_force_cap", "AC2 force cap", test)
    if "query_hash_overflow_total" in fields:
        fails.append(
            "AC2: do not add query_hash_overflow_total to compiler_metrics_fields.inc "
            "(process atomic; append-at-tail only if a CompilerMetrics field is required)"
        )

    # ── AC3: documented keys ──
    must("#3020 AC3: security-posture schema-2534", "AC3", test)
    must("#3020 AC3: type-linear-commit-health schema-2613", "AC3", test)
    must("#3020 AC3: reload-recovery-playbook schema-2953", "AC3", test)
    must("engine:metrics-query-hash-overflow-3020", "AC3 suite", suite)

    # ── AC4: Soft / no second bus / wiring ──
    must("g_query_hash_force_cap.load", "AC4", ev)
    must("no second metrics bus", "AC4", ev)
    if "AgentRegistry" in ev[ev.find("Issue #3020") : ev.find("Issue #3020") + 4000]:
        fails.append("AC4: must not introduce AgentRegistry")
    must("check_query_hash_overflow_3020", "AC4 build", build)
    must("cmd_query_hash_overflow_3020", "AC4 build cmd", build)

    # ── AC5: soak + no invented test/docs ──
    must("#3020 AC5: soak — no query_hash_overflow_total bump under default catalog", "AC5 soak", test)
    for rel in (
        "tests/compiler/test_issue_3020.cpp",
        "tests/core/test_issue_3020.cpp",
    ):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #81967")
    if _read("docs/design/3020-query-hash-overflow.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("aura_query_hash_overflow_total", "AC5 C ABI", jit)

    if fails:
        print(f"Issue #3020 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3020 domain query hash overflow fail-soft — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
