#!/usr/bin/env python3
"""Issue #4238: AgentScope multi-agent mutate stays Serialized without
region_keys — spawn apply bypasses decide_isolation (P1 orch residual).

Hole: parallel_intend / compose_supervised_batch call the decide_isolation
SSOT and can deny under production via parallel_require_region_keys_deny
(#3243/#3353), but the long-lived Scope spawn path deliberately does not
(#3728: "spawn is not a batch"). AgentScope::spawn admitted N mutate agents
with default region_key=0, so production hosts saw parallel fibers while
every mutate apply serialized on the per-Evaluator agent_apply_mu_
(gate_spawn_apply_region). AgentScope::observe_isolation (#3803) only
OBSERVED region_key_missing on join/workflow hashes — no admit deny closed
the loop, so high-frequency multi-agent parallel mutate was not closed.

Fix shape (no second isolation model; keys are never auto-invented):
  - AgentScope::spawn hosts the admit gate BEFORE handles_.emplace_back:
    region_key_missing_admit_deny_unlocked_ builds the candidate
    observation (specs_ region_keys + the incoming spec) through the SAME
    decide_isolation + region_key_missing_serialized predicate
    parallel_intend uses, adds the issue's ≥2 mutate-agent floor, and
    hands to the shared #3353 deny face (env escape
    AURA_PARALLEL_REQUIRE_REGION_KEYS / explicit 0 off-switch).
  - Deny face: typed failed handle (ok=false, error cites #4238,
    deny_class=Other, quota_dimension=kSerializedReasonMissingOrOverlapKeys),
    not emplaced; counter reuses region_key_missing_serialized_total —
    no new query key.
  - Soft / Off: one production_defaults_active load and out — observation
    stays zero-cost; no getenv on the Soft path.
  - #3803 observe_isolation keeps its join/workflow observation face;
    RestartN re-spawn (try_restart_from_spec_) re-checks the same
    predicate over specs_ as stored (#4281 — no second candidate, or a
    single keyless agent looks like two). Deny keeps the husk, bumps
    the existing counters, and does not burn max_restarts. adopt()
    untouched; orch:scope-spawn surfaces the deny through the existing
    !handle.ok typed-reject mapping (#3366).

Contract (one row per AC):
  AC1  agent_scope.h spawn() hosts the admit gate before emplace: cites
       #4238, calls region_key_missing_admit_deny_unlocked_, returns the
       typed failed handle (deny_class Other +
       kSerializedReasonMissingOrOverlapKeys) and bumps the existing
       region_key_missing_serialized_total counter
  AC2  SSOT reuse only: the admit helper window calls decide_isolation +
       region_key_missing_serialized + parallel_require_region_keys_deny
       and agent_scope.h never re-derives the level ternary (no
       IsolationLevel:: assignment); ≥2 mutate floor present
  AC3  Soft/Off zero-cost + escapes: helper exits on the production load
       before any env read, parallel_orch.h keeps the env escape names,
       no AgentRegistry, no key synthesis (auto_region_key/next_region_key
       absent), observe_isolation_unlocked_ deny-free (observation face)
  AC4  runtime ACs dispatched in tests/orch/test_agent_scope.cpp
       (ac4238_region_key_admit_deny in run_test_agent_scope, AC1–AC7
       labels); no tests/**/test_issue_4238.cpp; no docs/design/4238-*;
       RestartN calls the no-arg admit helper before replace (#4281)
  AC5  build.py wiring + scripts/coverage/root_check_allowlist.txt entry

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def window(src: str, begin: str, end: str, label: str, width: int = 7200) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + width)]

    scope_src = _read("src/orch/agent_scope.h")
    orch_src = _read("src/serve/parallel_orch.h")
    prim_src = _read("src/compiler/evaluator_primitives_agent.cpp")
    test_src = _read("tests/orch/test_agent_scope.cpp")
    build_src = _read("build.py")
    allow_src = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: admit gate hosted in spawn(), typed deny, counter reuse ──
    gate = window(
        scope_src,
        "Issue #4238: close the #3803 observation→admit loop",
        "handles_.emplace_back(spawn_agent_with_mailbox(*sched_, spec))",
        "AC1",
    )
    must("region_key_missing_admit_deny_unlocked_(spec)", "AC1", gate)
    must("region_key_missing_serialized_total", "AC1", gate)
    must('"AgentScope: region-key-missing spawn deny (#4238)"', "AC1", gate)
    must("AgentDenyClass::Other", "AC1", gate)
    must("kSerializedReasonMissingOrOverlapKeys", "AC1", gate)
    must("return failed;", "AC1", gate)
    # Gate sits before emplace inside spawn(): the window only exists when
    # the comment anchor precedes the emplace anchor.
    if not gate:
        fails.append("AC1: spawn() admit-gate window not found before emplace")
    helper = window(
        scope_src,
        "Issue #4238: candidate-admit face of the #3803 isolation SSOT",
        "live_handle_count_unlocked_",
        "AC1-helper",
    )
    must("region_key_missing_admit_deny_unlocked_", "AC1-helper", scope_src)

    # ── AC2: SSOT reuse — no second isolation ternary ──
    must("serve::parallel_orch::decide_isolation", "AC2", helper)
    must("serve::parallel_orch::region_key_missing_serialized", "AC2", helper)
    must("serve::parallel_orch::parallel_require_region_keys_deny", "AC2", helper)
    must("mutate_n < 2", "AC2", helper)
    absent("IsolationLevel::", "AC2", helper)
    absent("d.level =", "AC2", helper)
    absent("pure_mode ? ", "AC2", helper)

    # ── AC3: Soft/Off zero-cost, env escape, no registry / synthesis ──
    must("if (!production)\n            return false;", "AC3", helper)
    must("AURA_PARALLEL_REQUIRE_REGION_KEYS", "AC3", orch_src)
    must("parallel_require_region_keys_explicit_off", "AC3", orch_src)
    absent("class AgentRegistry", "AC3", scope_src)
    absent("auto_region_key", "AC3", scope_src)
    absent("next_region_key", "AC3", scope_src)
    observe = window(
        scope_src,
        "Issue #3803: decide_isolation over specs_ region_keys (SSOT)",
        "Issue #4238: candidate-admit face",
        "AC3-observe",
    )
    must("observe_isolation_unlocked_", "AC3-observe", observe)
    absent("admit_deny", "AC3-observe", observe)

    # ── AC4: runtime ACs dispatched; no invented artifacts ──
    must("static void ac4238_region_key_admit_deny()", "AC4", test_src)
    must("ac4238_region_key_admit_deny();", "AC4", test_src)
    for ac in ("4238 AC1", "4238 AC2", "4238 AC3", "4238 AC4", "4238 AC5", "4238 AC6", "4238 AC7"):
        must(ac, "AC4", test_src)
    must("try_restart_from_spec_", "AC4", scope_src)
    if _read("tests/orch/test_issue_4238.cpp"):
        fails.append("AC4: tests/orch/test_issue_4238.cpp must not exist")
    if _read("docs/design/4238-scope-region-admit.md"):
        fails.append("AC4: docs/design/4238-* must not exist")
    # RestartN (#4281) re-checks stored specs_ before replace. The
    # no-arg overload does not append the candidate a second time.
    restart = window(
        scope_src,
        "bool try_restart_from_spec_(std::size_t i, const AgentFailurePolicy& policy) noexcept",
        "void merge_directory_under_guard_",
        "AC4-restart",
    )
    must("spawn_agent_with_mailbox(*sched_, specs_[i])", "AC4-restart", restart)
    must("region_key_missing_admit_deny_unlocked_()", "AC4-restart", restart)
    absent("region_key_missing_admit_deny_unlocked_(spec", "AC4-restart", restart)
    absent("bypasses this gate by design", "AC4-restart", scope_src)
    hi = restart.find("region_key_missing_admit_deny_unlocked_()")
    sp = restart.find("spawn_agent_with_mailbox")
    if hi == -1 or sp == -1 or hi > sp:
        fails.append("AC4-restart: admit helper must run before spawn_agent_with_mailbox")
    pre = restart[hi:sp] if hi != -1 and sp != -1 and hi < sp else ""
    must("region_key_missing_serialized_total", "AC4-restart", pre)
    must("agent_restart_spawn_denied_total", "AC4-restart", pre)
    must("last_restart_deny_class_ = AgentDenyClass::Other", "AC4-restart", pre)
    must("return false;", "AC4-restart", pre)
    absent("handles_[i]", "AC4-restart", pre)
    ref = window(
        scope_src,
        "bool region_key_missing_admit_deny_unlocked_(const AgentSpec& spec) const",
        "bool region_key_missing_admit_deny_unlocked_() const",
        "AC1-ref",
    )
    must("region_key_missing_stored_deny_unlocked_(&spec)", "AC1-ref", ref)
    noarg = window(
        scope_src,
        "bool region_key_missing_admit_deny_unlocked_() const",
        "live_handle_count_unlocked_",
        "AC4-noarg",
    )
    must("region_key_missing_stored_deny_unlocked_(nullptr)", "AC4-noarg", noarg)
    absent("extra->region_key", "AC4-noarg", noarg)
    must("static void ac4281_restart_region_key_recheck()", "AC4", test_src)
    must("ac4281_restart_region_key_recheck();", "AC4", test_src)
    for ac in ("4281 AC1", "4281 AC2", "4281 AC3", "4281 AC4", "4281 AC5"):
        must(ac, "AC4", test_src)
    if _read("tests/orch/test_issue_4281.cpp"):
        fails.append("AC4: tests/orch/test_issue_4281.cpp must not exist")
    if _read("docs/design/4281-restart-region-admit.md"):
        fails.append("AC4: docs/design/4281-* must not exist")
    # Prim mapping the Aura face rides on stays intact.
    must("!handle.ok", "AC4-prim", prim_src)

    # ── AC5: wiring ──
    must("check_scope_region_admit_4238", "AC5", build_src)
    must("check_scope_region_admit_4238.py", "AC5", allow_src)

    if fails:
        print("check_scope_region_admit_4238: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_scope_region_admit_4238: ok (AC1–AC5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
