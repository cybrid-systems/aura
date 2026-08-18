#!/usr/bin/env python3
"""Issue #3142: SessionBound grant revoke cascade — nested TenantScope
abort + fiber steal paths must revoke inner SessionBound grants; stolen
flag prevents caller-side double-consume.

Contract (one row per AC):
  AC1  nested TenantScope RAII ctor captures (tenant, mid, fiber_id) snapshot;
       dtor calls revoke_session_grants_for(tenant, mid, fiber_id) BEFORE
       restoring prior principal (cascading revoke in ctor-reverse order).
  AC2  Fiber steal (evaluator_fiber_mutation.cpp:outermost_exit) marks
       stolen SessionBound entries as stolen flag; subsequent
       check_and_record_effect for the same stolen entry from caller side
       fails (effects_for_locked excludes stolen entries; consume loop
       skips stolen entries).
  AC3  Long-run chaos — 1000 nested TenantScope aborts leave
       g_capability_registry().session_bound_entries_alive() count == 0
       after outermost dtor (no leak).
  AC4  Additive metrics only — session_bound_revoked_on_scope_dtor_total +
       session_bound_revoked_on_steal_total + session_bound_orphan_detected_total.
       No schema change to existing counters or query keys.
  AC5  Source-cite capability_model.hh + evaluator_security.cpp +
       evaluator_fiber_mutation.cpp; extend tests/core/test_capability_single_use_consume.cpp
       with nested-abort + steal scenarios; no docs/design/, no
       tests/issues/test_issue_3142.cpp (per #81967/#1655).

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

    meta = _read("src/core/capability_model.hh")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")
    manifest = _read("scripts/coverage/manifests/3142.json")

    # ── AC1: nested TenantScope abort cascade-revoke ────────────────
    must("Issue #3142", "AC1 cite in meta", meta)
    must("revoke_session_grants_for(", "AC1 revoke overload (takes 3 args)", meta)
    must("scope-dtor-cascade", "AC1 dtor-cascade reason", meta)
    must("session_bound_revoked_on_scope_dtor_total", "AC1 dtor counter", meta)
    must("session_bound_entries_alive(", "AC1 live accessor", meta)

    # AC1 wire-up in evaluator_security.cpp (TenantScope::release)
    must("Issue #3142", "AC1 cite in eval_sec", eval_sec)
    must("revoke_session_grants_for(", "AC1 wire-up in TenantScope::release", eval_sec)
    must("scope-dtor-cascade", "AC1 reason in TenantScope::release", eval_sec)

    # ── AC2: fiber steal marks stolen → caller check fails ──────────
    must("bool stolen = false", "AC2 stolen flag on CapabilityGrant", meta)
    must("mark_session_bound_stolen(", "AC2 mark helper", meta)
    must("session_bound_revoked_on_steal_total", "AC2 steal counter", meta)
    # effects_for excludes stolen (so caller check fails)
    must("g.stolen", "AC2 stolen exclusion in effects_for / effects_for_locked", meta)
    # consume loop skips stolen
    must("g.stolen", "AC2 stolen skip in check_and_record_effect consume loop", meta)

    # AC2 wire-up in evaluator_fiber_mutation.cpp
    must("mark_session_bound_stolen", "AC2 mark helper wired in fiber_mutation", fiber_mut)
    must("Issue #3142", "AC2 cite in fiber_mutation", fiber_mut)

    # ── AC3: long-run chaos (no orphan detection) ───────────────────
    must("session_bound_orphan_detected_total", "AC3 orphan counter present (defensive)", meta)

    # ── AC4: additive metrics (struct END per #2906) ────────────────
    must("session_bound_revoked_on_scope_dtor_total", "AC4 dtor counter", meta)
    must("session_bound_revoked_on_steal_total", "AC4 steal counter", meta)
    must("session_bound_orphan_detected_total", "AC4 orphan counter", meta)
    # Appended at struct END — must come after capability_wildcard_write_fence_deny_total
    # (which was the latest addition before #3142)
    assert "capability_wildcard_write_fence_deny_total{0};" in meta, "AC4: counter before #3142 missing"
    pos_3141 = meta.find("capability_wildcard_write_fence_deny_total{0};")
    pos_3142 = meta.find("session_bound_revoked_on_scope_dtor_total{0};")
    if pos_3141 == -1 or pos_3142 == -1 or pos_3142 <= pos_3141:
        fails.append("AC4: #3142 counters not appended at struct END per #2906")

    # ── AC5: source-cite + extend test + no docs/issues ─────────────
    must("ac3142_1_nested_abort_cascade_revoke", "AC5 AC1 test function", test)
    must("ac3142_2_steal_marks_no_double_consume", "AC5 AC2 test function", test)
    must("ac3142_3_long_run_no_leak", "AC5 AC3 test function", test)
    must("ac3142_4_additive_metrics_and_source_cite", "AC5 AC4 test function", test)
    must("Issue #3142", "AC5 #3142 cite in test", test)
    must("Issue #2586", "AC5 prior #2586 still passing (additive)", test)

    # AC5: build.py wires linter
    must("check_session_bound_dtor_cascade.py", "AC5 build.py wires linter", build)

    # AC5: manifest exists and contains #3142
    if "3142" not in manifest:
        fails.append("AC5: manifest 3142.json missing '3142'")
    if "check_session_bound_dtor_cascade.py" not in manifest:
        fails.append("AC5: manifest 3142.json missing linter name")

    # AC5: no docs/design/, no tests/issues/test_issue_3142.cpp
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3142-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "issues" / "test_issue_3142.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3142.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3142.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3142.cpp present (forbidden per #81967)")

    if fails:
        print("FAIL: Issue #3142 linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3142 — SessionBound grant revoke cascade (nested TenantScope abort + fiber steal).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
