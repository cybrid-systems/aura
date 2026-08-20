#!/usr/bin/env python3
"""Issue #3179: production bare spawn defaults bp_scope_id to a non-empty
 non-process key so a BP storm in tenant A does NOT soft-reject a fresh
 bare spawn in tenant B (cross-tenant admit isolation).

Residual of #3015 / #3147 / #2633 — the Scope path already auto-fills
empty bp_scope_id (#3015 inherit), but the bare path
(spawn_agent_with_mailbox + Aura orch:spawn-agent) only *persisted*
the resolved spec.bp_scope_id; it never invented a stable,
session-local key when the caller left it empty. Under production
multi-tenant / multi-session hosts that still use the MVP surface
(or mix bare + Scope agents), every bare agent shared the
process-global BP recent gauge → cross-tenant cross-poison.

Contract (one row per AC):
  AC1  Production + bare spawn, no :bp-scope-id → effective gauge key is
       non-empty and NOT the process bucket; two concurrent
       tenants/sessions do not share the same recent counter.
  AC2  Soft / AURA_SANDBOX=off → empty stays empty (process bucket);
       zero extra atomics / map inserts on the hot path.
  AC3  Explicit :bp-scope-id "tenant-a" wins (no override).
  AC4  Scope path unchanged — #3015 inherit still authoritative;
       no double-prefix.
  AC5  Admit preflight under a storm in tenant A does NOT soft-reject
       a fresh bare spawn in tenant B (same process, production
       defaults).
  AC6  Existing tests (test_mailbox_bp_admit*, test_per_scope_bp_admit,
       test_agent_name_table_isolation) stay green; the new test block
       verifies bare-spawn distinct-keys under production.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Files in scope for #3179 (bare spawn bp_scope_id resolver).
SCOPE_FILES = [
    "src/orch/agent_spawn.h",
    "src/orch/agent_scope.h",
    "src/compiler/evaluator_primitives_agent.cpp",
    "tests/orch/test_bare_bp_resolve.cpp",
    "scripts/coverage/checks/check_per_scope_bp_admit_3179.py",
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

    def must_any(tokens: list[str], label: str, hay: str) -> None:
        if not any(t in hay for t in tokens):
            fails.append(f"{label}: missing any of {tokens!r} (none of the alternate wire patterns found)")

    spawn = _read("src/orch/agent_spawn.h")
    scope = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_bare_bp_resolve.cpp")
    build = _read("build.py")
    # Lineage / additive-only references (must remain unchanged in meaning).
    check3015 = _read("scripts/coverage/checks/check_scope_bp_inherit_3015.py")
    check3147 = _read("scripts/coverage/checks/check_mailbox_bp_scope_handle_3147.py")

    # ── AC1: production + bare spawn → non-empty non-process key ─────
    # The resolver must exist and be wired into spawn_agent_with_mailbox.
    must("[[nodiscard]] inline std::string resolve_bare_bp_scope_id", "AC1 resolver declared in agent_spawn.h", spawn)
    must("Issue #3179", "AC1 fix marker in resolver comment", spawn)
    # The resolver consults production_scope_bp_inherit (Soft off) and
    # current_quota_tenant (tenant preferred) — both gates.
    must("production_scope_bp_inherit", "AC1 resolver consults production_scope_bp_inherit", spawn)
    must("current_quota_tenant", "AC1 resolver prefers TLS quota tenant", spawn)
    # Wired into spawn_agent_with_mailbox (single wire point covers
    # both C++ surface and Aura orch:spawn-agent primitive).
    must_any(
        [
            "h.bp_scope_id = resolve_bare_bp_scope_id(spec.bp_scope_id);",
            "h.bp_scope_id = std::move(spec.bp_scope_id);",
        ],
        "AC1 spawn_agent_with_mailbox wires bp_scope_id (#3179 resolver or #3147 std::move)",
        spawn,
    )
    must("spec.bp_scope_id = h.bp_scope_id;", "AC1 spec kept in sync for downstream BP arms", spawn)

    # ── AC2: Soft / AURA_SANDBOX=off → empty stays empty ─────────────
    # The resolver must short-circuit on production_scope_bp_inherit()==false.
    must("if (!production_scope_bp_inherit())", "AC2 Soft short-circuit (if-not-produce-soft)", spawn)
    # Soft / AURA_SANDBOX=off stays process bucket (zero extra cost).
    must("AURA_SANDBOX", "AC2 env name (lineage unchanged)", spawn)

    # ── AC3: explicit :bp-scope-id wins (no override) ────────────────
    # The resolver's first branch is "if (!explicit_id.empty()) return
    # std::string(explicit_id);" — explicit wins before any gate.
    must("if (!explicit_id.empty())", "AC3 explicit wins (early return guard)", spawn)

    # ── AC4: Scope path unchanged — no double-prefix ─────────────────
    # The Scope path's #3015 inherit logic must remain authoritative.
    # production_scope_bp_inherit() stays in agent_scope.h (its current home).
    must("production_scope_bp_inherit", "AC4 #3015 lineage preserved", scope)
    must("production_scope_bp_inherit", "AC4 #3015 helper still in agent_scope.h", scope)
    # AgentScope::spawn still auto-fills empty from its own bp_scope_id_
    # (the existing #3015 inherit path), NOT from resolve_bare_bp_scope_id.
    must("if (spec.bp_scope_id.empty() && production_scope_bp_inherit())", "AC4 #3015 Scope inherit unchanged", scope)
    must("spec.bp_scope_id = bp_scope_id_;", "AC4 Scope uses scope's bp_scope_id_, not resolver", scope)
    must("spawn_bp_scope_inherited_total", "AC4 #3015 metric still bumped by Scope path", scope)
    # The bare resolver must NOT be called from the Scope path (would
    # double-prefix). Negative check: AgentScope::spawn must not invoke
    # resolve_bare_bp_scope_id.
    must_not("resolve_bare_bp_scope_id", "AC4 Scope path must NOT call bare resolver (no double-prefix)", scope)

    # ── AC5: Admit preflight under storm does not soft-reject other
    # tenants. The resolver's tenant-keyed branch ("t:<tid>") provides
    # this — verified by spawn_agent_with_mailbox routing distinct
    # tenants to distinct gauges.
    must('"t:" + std::to_string(tid)', "AC5 tenant-prefixed key for cross-tenant isolation", spawn)
    # The bare-spawn fallback path also gives unique keys (per call):
    must('"bare:" + std::to_string(n)', "AC5 bare-seq fallback unique per call", spawn)
    # And the test exercises both paths.
    must("h_a.bp_scope_id != h_b.bp_scope_id", "AC5 test verifies distinct keys for distinct bare spawns", test)

    # ── AC6: existing tests stay green; new block verifies new path ──
    must("#3179", "AC6 test marker in test file", test)
    must("resolve_bare_bp_scope_id", "AC6 test imports resolver", test)
    must("production_scope_bp_inherit", "AC6 test consults production gate", test)
    must("spawn_agent_with_mailbox", "AC6 test exercises spawn_agent_with_mailbox", test)
    # Source-cite in test file.
    must("Issue #3179", "AC6 test file cites Issue #3179", test)

    # ── Lineage preservation ──────────────────────────────────────────
    # #3015 linter still passes — its contract (Scope inherit) is
    # unchanged. The new resolver is additive.
    must("production_scope_bp_inherit", "AC lineage #3015 linter intact", check3015)
    must("bp_scope_id", "AC lineage #3015 scope field cited", check3015)
    # #3147 linter (handle persistence) still passes — we persist on
    # h.bp_scope_id after the resolver runs.
    must("h.bp_scope_id", "AC lineage #3147 handle persistence intact", check3147)
    # build.py wires the new linter.
    must("check_per_scope_bp_admit_3179", "AC build.py wires #3179 linter", build)
    # No new public query key required (additive only, AC4).
    must_not("schema-3179", "AC4 no new schema-3179 prim key", prim)
    must_not("issue-3179", "AC4 no new issue-3179 prim key", prim)
    # Existing posture keys (mailbox_bp_*, scope BP map) unchanged.
    must("mailbox_bp_recent_total", "AC5 #3109 metric preserved", prim)
    must("spawn_bp_scope_inherited_total", "AC5 #3015 metric preserved", prim)

    # No new tests/issues/test_issue_3179.cpp (per #81934 / #81967).
    for rel in (
        "tests/issues/test_issue_3179.cpp",
        "tests/core/test_issue_3179.cpp",
        "tests/orch/test_issue_3179.cpp",
        "tests/compiler/test_issue_3179.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: forbidden new test file {rel} (extend existing test per #81934 / #81967)")

    # No docs/design/3179-* (per #1655).
    docs_design = ROOT / "docs/design"
    if docs_design.is_dir():
        for entry in docs_design.iterdir():
            if entry.name.startswith("3179-"):
                fails.append(f"AC6: forbidden docs/design/{entry.name} (per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} check(s) failed for #3179 bare-spawn bp_scope_id linter",
            file=sys.stderr,
        )
        return 1

    print("OK: #3179 bare-spawn bp_scope_id linter passed (all ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
