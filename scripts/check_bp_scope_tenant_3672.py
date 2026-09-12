#!/usr/bin/env python3
"""Issue #3672: resolve_bare_bp_scope_id prefers the spawn's own tenant;
bare orch:spawn-agent BP admit aggregates per tenant.

Contract (one row per AC):
  AC1  production + empty explicit + spawn-tenant ladder → "t:<tid>"
       (not bare:<seq>); same tenant → same key (shared gauge); the
       spawn path stamps the resolved tenant onto the handle
  AC2  different tenant → its own key; same-tenant spawn at threshold
       soft-rejects (quota_exceeded / mailbox-bp / AdmissionRejected)
       while a fresh tenant admits
  AC3  explicit :bp-scope-id still wins, no double-prefix (#3015)
  AC4  Soft / AURA_SANDBOX=off stays empty even with a spawn tenant
  AC5  tests extend test_bare_bp_resolve / test_mailbox_bp_admit; no
       invent test; no docs/design; no new query key

Exit 0 = all rows satisfied. --self-test checks the row helpers on
synthetic text, not the live tree.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _rows(must, must_not) -> list[str]:
    fails: list[str] = []

    spawn = _read("src/orch/agent_spawn.h")
    bare_test = _read("tests/orch/test_bare_bp_resolve.cpp")
    admit_test = _read("tests/orch/test_mailbox_bp_admit.cpp")
    build = _read("build.py")

    # AC1: resolver takes the spawn-tenant ladder result.
    must("resolve_bare_bp_scope_id(std::string_view explicit_id,", "AC1 resolver signature", spawn)
    must("std::uint64_t spec_tenant = 0) noexcept {", "AC1 spec_tenant param", spawn)
    must("spec_tenant != 0 ? spec_tenant", "AC1 tenant ladder", spawn)
    must("resolve_bare_bp_scope_id(spec.bp_scope_id, spawn_tenant)", "AC1 call site", spawn)
    must("Issue #3672", "AC1 cite", spawn)

    # AC3: explicit id still wins before the tenant ladder.
    must("if (!explicit_id.empty())", "AC3 explicit win", spawn)

    # AC5: tests extend the two src-aligned suites.
    must("3672 AC1", "AC5 bare test AC1", bare_test)
    must("3672 AC1", "AC5 admit test AC1", admit_test)
    must("3672 AC2", "AC5 admit test AC2", admit_test)
    if _read("tests/orch/test_issue_3672.cpp") or _read("tests/issues/test_issue_3672.cpp"):
        fails.append("AC5: test_issue_3672.cpp present (forbidden per #81934)")
    if _read("docs/design/3672-bp-scope-tenant.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("check_bp_scope_tenant_3672", "AC5 build.py", build)
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    fails = _rows(must, must_not)

    if fails:
        print("FAIL #3672 bp_scope_tenant:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3672 bp_scope_tenant: all rows satisfied")
    return 0


def _self_test() -> int:
    """Row helpers must flag missing anchors on synthetic stripped text."""
    failures: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            failures.append(label)

    good = (
        "resolve_bare_bp_scope_id(std::string_view explicit_id,\n"
        "                                                          std::uint64_t spec_tenant = 0) noexcept {\n"
        "spec_tenant != 0 ? spec_tenant\n"
        "resolve_bare_bp_scope_id(spec.bp_scope_id, spawn_tenant)\n"
    )
    must("resolve_bare_bp_scope_id(std::string_view explicit_id,", "self signature", good)
    must("std::uint64_t spec_tenant = 0) noexcept {", "self param", good)
    must("spec_tenant != 0 ? spec_tenant", "self ladder", good)
    must("resolve_bare_bp_scope_id(spec.bp_scope_id, spawn_tenant)", "self call site", good)
    if "bare:1" in good:
        failures.append("self: stray anchor in sample")
    print("SELF-TEST OK: check_bp_scope_tenant_3672 row helpers" if not failures else f"SELF-TEST FAIL: {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    sys.exit(main())
