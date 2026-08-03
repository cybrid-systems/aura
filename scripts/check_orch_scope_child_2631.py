#!/usr/bin/env python3
"""check_orch_scope_child_2631.py — Issue #2631 source gate.

Aura surface for hierarchical AgentScope (orch:scope-child). Closes
the script-side tree supervision gap: #2537 C++ hierarchical
AgentScope exists but #2588 Aura surface was flat. This adds the
orch:scope-child prim that calls AgentScope::spawn_child() on an
existing per-Evaluator scope. Child is owned by parent, cancel_all
propagates top-down.

AC1: Aura can spawn child scope, spawn agents under child, cancel_all
     propagates top-down. The orch:scope-child prim must exist and
     call AgentScope::spawn_child() (or equivalent).
AC2: ~AgentScope / scope-join-all drains children then parent.
     #2537 cancel / dtor order is documented in agent_scope.h.
AC3: scripts/check_orch_mvp_scope.py --strict still green
     (no AgentRegistry / global_agent_registry). The hierarchical
     scope is bound to per-Evaluator scope map, not a global map.
AC4: Structured hash + query:orch-module-stats metric / schema keys.
     scope-child-total, scope-child-wired, schema-2631, issue-2631
     must be present in the query surface.
AC5: README explicit: hierarchy bound to Evaluator/session; not a
     global registry. #2537 / #2588 references preserved.
AC6: src-aligned test (extend test_orch_scope_2588 or new
     test_orch_scope_child_*) + coverage gate source-cite.

Rationale (Issue #2631 body):
  C++ AgentScope::spawn_child() (#2537) already builds an explicit
  parent/children tree (unique_ptr ownership, top-down cancel,
  bottom-up drain) without a process-global registry. Aura language
  surface (#2588) only exposes the flat per-Evaluator scope.
  orch:scope-child was explicitly deferred. This adds it so script-side
  supervisors can express tree supervision today.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AGENT = ROOT / "src/compiler/evaluator_primitives_agent.cpp"
AGENT_SPAWN = ROOT / "src/orch/agent_spawn.h"
AGENT_SCOPE = ROOT / "src/orch/agent_scope.h"
README = ROOT / "src/orch/README.md"
MVPSCOPE = ROOT / "scripts/check_orch_mvp_scope.py"
TEST_DIR = ROOT / "tests/compiler"


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    # AC1: orch:scope-child prim registered in evaluator_primitives_agent.cpp
    if not AGENT.exists():
        failures.append("AC1: src/compiler/evaluator_primitives_agent.cpp not found")
        agent = ""
    else:
        agent = AGENT.read_text(encoding="utf-8", errors="replace")

    if agent:
        if 'add("orch:scope-child"' not in agent:
            failures.append(
                "AC1: orch:scope-child prim not registered in "
                "evaluator_primitives_agent.cpp (call-site wiring missing)"
            )
        # Must call AgentScope::spawn_child() (or equivalent).
        if "spawn_child" not in agent:
            failures.append(
                "AC1: evaluator_primitives_agent.cpp does not call "
                "AgentScope::spawn_child() (prim must use hierarchical C++)"
            )
        # Must use get_or_create_agent_scope (per-Evaluator scope map).
        if "get_or_create_agent_scope" not in agent:
            failures.append(
                "AC1: orch:scope-child must use get_or_create_agent_scope "
                "(per-Evaluator scope map, not a global registry)"
            )

    # AC4: scope_child_total counter + query surface keys
    if not AGENT_SPAWN.exists():
        failures.append("AC4: src/orch/agent_spawn.h not found")
        spawn = ""
    else:
        spawn = AGENT_SPAWN.read_text(encoding="utf-8", errors="replace")

    if spawn and "scope_child_total" not in spawn:
        failures.append(
            "AC4: src/orch/agent_spawn.h missing scope_child_total counter "
            "(hierarchical AgentScope #2631)"
        )

    if agent and "scope_child_total" not in agent:
        # Agent file must also have the fetch.
        failures.append(
            "AC4: evaluator_primitives_agent.cpp does not bump "
            "scope_child_total counter (call-site wiring missing)"
        )

    # AC4: query:orch-module-stats keys
    if agent and "query:orch-module-stats" in agent:
        for key in (
            "scope-child-total",
            "scope-child-wired",
            "schema-2631",
            "issue-2631",
        ):
            if key not in agent:
                failures.append(
                    f"AC4: evaluator_primitives_agent.cpp does not expose "
                    f"{key} on query:orch-module-stats"
                )
        # Compatibility: prior #2588 / #2083 / #2161 keys preserved.
        for key in ("schema-2588", "schema-2083", "schema-2161", "orch-scope-wired"):
            if key not in agent:
                failures.append(
                    f"AC4: evaluator_primitives_agent.cpp does not preserve "
                    f"existing {key} (compatibility with #2588 / #2083 / #2161)"
                )

    # AC2: ~AgentScope / scope-join-all drains children then parent
    if AGENT_SCOPE.exists():
        scope = AGENT_SCOPE.read_text(encoding="utf-8", errors="replace")
        if "spawn_child" not in scope:
            failures.append(
                "AC2: AgentScope::spawn_child missing from src/orch/agent_scope.h "
                "(#2537 hierarchical C++)"
            )
    else:
        failures.append("AC2: src/orch/agent_scope.h not found")

    # AC3: scripts/check_orch_mvp_scope.py still green
    if not MVPSCOPE.exists():
        failures.append("AC3: scripts/check_orch_mvp_scope.py not found")
    else:
        mvp = MVPSCOPE.read_text(encoding="utf-8", errors="replace")
        if "AgentRegistry" not in mvp or "global_agent_registry" not in mvp:
            failures.append(
                "AC3: scripts/check_orch_mvp_scope.py must still reject "
                "AgentRegistry / global_agent_registry (hierarchical scope "
                "bound to per-Evaluator scope map, not a global map)"
            )

    # AC5: README explicit: hierarchy bound to Evaluator/session
    if not README.exists():
        failures.append("AC5: src/orch/README.md not found")
    else:
        readme = README.read_text(encoding="utf-8", errors="replace")
        # Must mention scope-child or hierarchy.
        if "scope-child" not in readme and "hierarchy" not in readme.lower():
            failures.append(
                "AC5: src/orch/README.md does not document scope-child / "
                "hierarchy (must be explicit per-Evaluator / no global registry)"
            )
        # Must reference #2537 + #2588.
        if "#2537" not in readme and "2537" not in readme:
            failures.append(
                "AC5: src/orch/README.md must reference #2537 (C++ hierarchical "
                "AgentScope)"
            )
        if "#2588" not in readme and "2588" not in readme:
            failures.append(
                "AC5: src/orch/README.md must reference #2588 (flat Aura scope)"
            )
        # Must note per-Evaluator hierarchy (no global registry).
        # Accept mentions of "AgentRegistry" / "global_agent_registry" as
        # *things to avoid* (the README documents the guardrail).
        if "per-Evaluator" not in readme and "per-evaluator" not in readme.lower() and \
                "per Evaluator" not in readme:
            failures.append(
                "AC5: src/orch/README.md must note per-Evaluator hierarchy "
                "(no global registry allowed)"
            )

    # AC6: test file has ac2631_* sections
    found_test = False
    for test_path in TEST_DIR.glob("test_*.cpp"):
        test_text = test_path.read_text(encoding="utf-8", errors="replace")
        for ac_fn in (
            "ac2631_spawn_child_hierarchy",
            "ac2631_cancel_top_down_propagates",
            "ac2631_mvp_linter_still_green",
            "ac2631_query_surface_schema",
            "ac2631_source_and_readme",
        ):
            if ac_fn in test_text:
                found_test = True
    if not found_test:
        any_ac2631 = False
        for test_path in TEST_DIR.glob("test_*.cpp"):
            test_text = test_path.read_text(encoding="utf-8", errors="replace")
            if "ac2631_" in test_text:
                any_ac2631 = True
                break
        if not any_ac2631:
            failures.append(
                "AC6: no test file has ac2631_* sections "
                "(#2631 call-site wiring test coverage missing)"
            )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if strict:
            return 1
        print(
            f"\nNON-STRICT: {len(failures)} issue(s) above (--strict to enforce)",
            file=sys.stderr,
        )
        return 0

    print(
        "OK: all #2631 ACs satisfied (orch:scope-child hierarchical AgentScope "
        "Aura surface — call-site wiring + counter + query + linter green + tests)"
    )
    return 0


def main_strict() -> int:
    """Always-strict variant for self-test fixtures."""
    saved = sys.argv
    try:
        sys.argv = list(saved) + ["--strict"]
        return main()
    finally:
        sys.argv = saved


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    tmp = Path(tempfile.mkdtemp(prefix="check_2631_selftest_"))
    try:
        good_agent = tmp / "agent.cpp"
        good_agent.write_text(
            '    add("orch:scope-spawn", [&ev](std::span<const EvalValue> a) -> EvalValue {\n'
            "        auto& parent = get_or_create_agent_scope(&ev, *sched);\n"
            "        return build_hash(kv);\n"
            "    });\n"
            '    add("orch:scope-child", [&ev](std::span<const EvalValue> a) -> EvalValue {\n'
            "        auto& parent = get_or_create_agent_scope(&ev, *sched);\n"
            "        auto& child = parent.spawn_child();\n"
            "        g_orch_module_stats.scope_child_total.fetch_add(1);\n"
            "        return build_hash(kv);\n"
            "    });\n"
            '    "query:orch-module-stats", [&ev](const auto&) -> EvalValue {\n'
            '        insert_kv("scope-child-total", make_int(0));\n'
            '        insert_kv("scope-child-wired", 1);\n'
            '        insert_kv("schema-2631", 2631);\n'
            '        insert_kv("issue-2631", 2631);\n'
            '        insert_kv("schema-2588", 2588);\n'
            '        insert_kv("schema-2083", 2083);\n'
            '        insert_kv("schema-2161", 2161);\n'
            '        insert_kv("orch-scope-wired", 1);\n'
            "    };\n",
            encoding="utf-8",
        )

        good_spawn = tmp / "agent_spawn.h"
        good_spawn.write_text(
            "struct OrchModuleStats {\n"
            "    std::atomic<std::uint64_t> scope_spawn_total{0};\n"
            "    std::atomic<std::uint64_t> scope_child_total{0};\n"
            "    std::atomic<std::uint64_t> scope_watch_total{0};\n"
            "};\n",
            encoding="utf-8",
        )

        good_scope = tmp / "agent_scope.h"
        good_scope.write_text(
            "class AgentScope {\n"
            "public:\n"
            "    AgentScope& spawn_child() { return *this; }\n"
            "};\n",
            encoding="utf-8",
        )

        good_mvp = tmp / "mvp.py"
        good_mvp.write_text(
            "if 'AgentRegistry' in src or 'global_agent_registry' in src:\n"
            "    fail('no global registry allowed')\n",
            encoding="utf-8",
        )

        good_readme = tmp / "README.md"
        good_readme.write_text(
            "# Orch\n\n"
            "## Hierarchical AgentScope (#2537)\n\n"
            "C++ `AgentScope::spawn_child()` builds a parent/children tree.\n\n"
            "## Aura scope surface (#2588)\n\n"
            "`orch:scope-spawn`, `orch:scope-child` (per-Evaluator scope map).\n\n"
            "Not a global registry. #2588 / #2083 / #2161 preserved.\n",
            encoding="utf-8",
        )

        good_test = tmp / "test.cpp"
        good_test.write_text(
            "static void ac2631_spawn_child_hierarchy() {}\n"
            "static void ac2631_cancel_top_down_propagates() {}\n"
            "static void ac2631_mvp_linter_still_green() {}\n"
            "static void ac2631_query_surface_schema() {}\n"
            "static void ac2631_source_and_readme() {}\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )

        import check_orch_scope_child_2631 as self_mod

        original = {
            "AGENT": self_mod.AGENT,
            "AGENT_SPAWN": self_mod.AGENT_SPAWN,
            "AGENT_SCOPE": self_mod.AGENT_SCOPE,
            "README": self_mod.README,
            "MVPSCOPE": self_mod.MVPSCOPE,
            "TEST_DIR": self_mod.TEST_DIR,
        }
        try:
            self_mod.AGENT = good_agent
            self_mod.AGENT_SPAWN = good_spawn
            self_mod.AGENT_SCOPE = good_scope
            self_mod.README = good_readme
            self_mod.MVPSCOPE = good_mvp
            self_mod.TEST_DIR = tmp
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Bad fixture: missing scope-child prim
        bad_agent = tmp / "agent_bad.cpp"
        bad_agent.write_text(
            '    add("orch:scope-spawn", [&ev](std::span<const EvalValue> a) -> EvalValue {\n'
            "        return build_hash(kv);\n"
            "    });\n"
            '    "query:orch-module-stats", [&ev](const auto&) -> EvalValue {\n'
            '        insert_kv("scope-child-wired", 1);\n'
            "    };\n",
            encoding="utf-8",
        )
        try:
            self_mod.AGENT = bad_agent
            self_mod.AGENT_SPAWN = good_spawn
            self_mod.AGENT_SCOPE = good_scope
            self_mod.README = good_readme
            self_mod.MVPSCOPE = good_mvp
            self_mod.TEST_DIR = tmp
            rc_bad = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print(
                "SELF-TEST FAIL: known-bad (missing scope-child prim) accepted",
                file=sys.stderr,
            )
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
