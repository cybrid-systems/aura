#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4139: cross-Evaluator observe-only token join — hosts can believe
# reclaim closed when the source still owes cleanup. join_via_handoff /
# orch:join-via-token never take ownership, never release the source
# reservation, never detach the source mailbox (observation_only /
# reservation_held_by_source), so an importer-visible Ok while the exported
# snapshot carries source_must_wait_reclaimed leaves the source Evaluator
# owing ensure_reclaimed_cleanup / Scope sweep / join. This is the
# capability boundary above single-Evaluator MVP for multi-agent long-run
# coordination — NOT fixed by a process-global AgentRegistry, and no
# ownership-moving saga is invented here. Issue #4282 records the
# product decision: stay observe-only; no ownership-transfer prim.
# Fix shape: document + production observability — the
# handoff_join_via_token_source_pending_total counter (appended at
# OrchModuleStats struct END, no mid-struct insert / no key rename) bumps
# when an observe result carries the pending-source snapshot; Soft/Off and
# the pending=false path stay zero-cost.
#
# AC1 - observe + doc: join_via_handoff cites Issue #4139 and its body
#       mirrors the source snapshot BEFORE the #4004 source-gone gate and
#       bumps the source-pending counter; the "NOT reclaim-complete" doc
#       anchor is present; runtime AC ac4139_1 pins the cross-Evaluator
#       soak (observe Ok while the source still owes cleanup until
#       ensure_reclaimed_cleanup).
# AC2 - counter additive at struct END: the new counter is declared after
#       the #4026 recv_proxy_denied_total (the prior last member), the
#       existing handoff counter pair is unchanged (no key rename), the
#       ensure_reclaimed_cleanup SSOT stays, the issue stamp is 4139, and
#       runtime AC ac4139_2 pins the source-gone observe face.
# AC3 - observe-only preserved / zero-cost: the bump is guarded by the
#       pending flag; the join_via_handoff body gains no ownership move
#       (no ensure/release/detach calls — it stays a read-only observer);
#       no process-global AgentRegistry; runtime AC ac4139_3 pins the
#       proxy recv-deny / send-ask liaison face.
# AC4 - hosting/wiring: 4 ac4139 runtime ACs defined AND called in
#       tests/orch/test_join_drain_reclaim.cpp, no
#       tests/orch/test_issue_4139.cpp (per #81934), no
#       docs/design/4139-* (per #1655), no new query key, the Aura stats
#       hash exposes the additive rows (no key rename), linter registered
#       in build.py and on the root check allowlist.
#
# Self-test:
#   python3 scripts/check_join_token_observe_4139.py
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _brace_slice(src: str, start: int, limit: int = 8000) -> str:
    """Return src[start:end] where end closes the first { after start."""
    open_brace = src.find("{", start)
    if open_brace < 0:
        return src[start : start + limit]
    depth = 0
    end = min(len(src), start + limit)
    for i in range(open_brace, min(len(src), open_brace + limit)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    return src[start:end]


def main() -> int:
    fails: list[str] = []

    spawn_raw = _read("src/orch/agent_spawn.h")
    spawn = _strip_cpp_comments(spawn_raw)
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    join_test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build_src = _read("build.py")
    allow_src = _read("scripts/coverage/root_check_allowlist.txt")

    # -- AC1: observe + doc (snapshot mirrors before the source-gone gate;
    #        bump present; NOT-reclaim-complete documented) --
    sig = spawn.find("JoinViaTokenResult join_via_handoff(const HandoffToken& tok")
    if sig < 0:
        fails.append("AC1: join_via_handoff definition missing")
    # Anchor the body slice at `noexcept {` — the signature carries a
    # `jp = {}` default argument whose empty braces would end a naive
    # first-brace walk immediately.
    body_open = spawn.find("noexcept {", sig) if sig >= 0 else -1
    if "Issue #4139" not in spawn_raw:
        fails.append("AC1: agent_spawn.h must cite Issue #4139")
    if "NOT reclaim-complete" not in spawn_raw:
        fails.append("AC1: NOT-reclaim-complete doc anchor missing")
    body = _brace_slice(spawn, body_open) if body_open >= 0 else ""
    if body:
        mirror = body.find("out.source_must_wait_reclaimed = tok.source_must_wait_reclaimed")
        gate = body.find("handoff_source_gone(tok.source_live)")
        if mirror < 0:
            fails.append("AC1: source snapshot mirror missing from join_via_handoff")
        elif gate < 0 or mirror > gate:
            fails.append("AC1: snapshot mirror must precede the #4004 source-gone gate")
        bump = body.find("handoff_join_via_token_source_pending_total.fetch_add")
        if bump < 0:
            fails.append("AC1: join_via_handoff must bump the source-pending counter")
        total = body.find("handoff_join_via_token_total.fetch_add")
        if total < 0 or (bump >= 0 and bump > total + 400):
            fails.append("AC1: source-pending bump must sit with the existing counter pair")
    else:
        fails.append("AC1: join_via_handoff body slice unavailable")
    if "ac4139_1_cross_evaluator_soak_observe_not_reclaim_complete" not in join_test:
        fails.append("AC1: cross-Evaluator soak runtime AC missing from the join-drain test")

    # -- AC2: counter additive at struct END / no rename / SSOT intact --
    recv_pos = spawn.find("recv_proxy_denied_total{0};")
    pend_pos = spawn.find("handoff_join_via_token_source_pending_total{0};")
    if recv_pos < 0 or pend_pos < 0 or recv_pos > pend_pos:
        fails.append("AC2: counter must be appended AFTER recv_proxy_denied_total (struct END)")
    for keep in (
        "handoff_join_via_token_total{0};",
        "handoff_join_via_token_timeout_total{0};",
        "wait_reclaimed_total{0};",
        "wait_reclaimed_timeout_total{0};",
    ):
        if keep not in spawn:
            fails.append(f"AC2: existing counter {keep} must stay (no key rename)")
    if "ensure_reclaimed_cleanup(AgentHandle& h)" not in spawn:
        fails.append("AC2: ensure_reclaimed_cleanup SSOT must stay in agent_spawn.h")
    if "kJoinTokenSourcePendingIssue = 4139" not in spawn:
        fails.append("AC2: issue stamp kJoinTokenSourcePendingIssue = 4139 missing")
    if "ac4139_2_source_sched_destroy_invalidates_observe_join" not in join_test:
        fails.append("AC2: source-gone observe runtime AC missing from the join-drain test")

    # -- AC3: observe-only preserved / zero-cost / no ownership move --
    if body:
        guard = body.find("if (out.source_must_wait_reclaimed)")
        if guard < 0 or (bump >= 0 and guard > bump):
            fails.append("AC3: the bump must be guarded by the pending flag (zero-cost)")
        for forbidden in (
            "ensure_reclaimed_cleanup(",
            "release_reservation_if_any(",
            "detach(",
            "abandon_reclaimed(",
            "maybe_force_release_reclaimed_quota(",
        ):
            if forbidden in body:
                fails.append(f"AC3: join_via_handoff must stay observe-only (no {forbidden} call)")
    else:
        fails.append("AC3: join_via_handoff body slice unavailable")
    if "ac4139_3_proxy_recv_denied_send_ask_ok" not in join_test:
        fails.append("AC3: proxy recv-deny / send-ask liaison runtime AC missing")
    if "class AgentRegistry" in spawn or "struct AgentRegistry" in spawn:
        fails.append("AC3: no process-global AgentRegistry")

    # -- AC4: hosting / wiring / no invent --
    defs = re.findall(r"static void (ac4139_\w+)\(\)", join_test)
    calls = re.findall(r"^\s{4}(ac4139_\w+)\(\);", join_test, re.M)
    if len(defs) != 4:
        fails.append(f"AC4: expected 4 ac4139 runtime ACs, found {len(defs)}")
    if set(defs) != set(calls):
        fails.append("AC4: ac4139 ACs must all be called from the batch runner")
    if (ROOT / "tests/orch/test_issue_4139.cpp").exists():
        fails.append("AC4: tests/orch/test_issue_4139.cpp must not exist (per #81934)")
    if list((ROOT / "docs" / "design").glob("4139-*")):
        fails.append("AC4: docs/design/4139-* must not exist (per #1655)")
    if "query:4139" in spawn_raw or "query:4139" in prim:
        fails.append("AC4: no new query key (query:4139 must not exist)")
    if "handoff-join-via-token-source-pending-total" not in prim:
        fails.append("AC4: Aura stats hash must expose handoff-join-via-token-source-pending-total")
    if "schema-4139" not in prim:
        fails.append("AC4: Aura stats hash must wire schema-4139")
    if "recv-proxy-denied-wired" not in prim or "handoff-join-via-token-total" not in prim:
        fails.append("AC4: prior Aura stats hash rows must stay (no key rename)")
    if "check_join_token_observe_4139" not in build_src:
        fails.append("AC4: linter must be registered in build.py")
    if "check_join_token_observe_4139.py" not in allow_src:
        fails.append("AC4: linter must be on the root check allowlist")

    # Issue #4282: the product ask is decided — keep the #4139 observe-only
    # ceiling. Do not add a transfer prim, a query key, or a new test file.
    if "Issue #4282" not in spawn_raw:
        fails.append("4282: agent_spawn.h must cite the observe-only product decision")
    if "observe-only HandoffToken is the multi-Evaluator ceiling" not in spawn_raw:
        fails.append("4282: decision must name the observe-only ceiling")
    if "no ownership-transfer prim" not in spawn_raw:
        fails.append("4282: decision must refuse an ownership-transfer prim")
    for forbidden in (
        "transfer_handoff_ownership",
        "ownership_transfer_prim",
        "move_handoff_ownership",
    ):
        if forbidden in spawn:
            fails.append(f"4282: {forbidden} must not exist (observe-only ceiling)")
    if (ROOT / "tests/orch/test_issue_4282.cpp").exists():
        fails.append("4282: tests/orch/test_issue_4282.cpp must not exist")
    if list((ROOT / "docs" / "design").glob("4282-*")):
        fails.append("4282: docs/design/4282-* must not exist")
    if "query:4282" in spawn_raw or "query:4282" in prim:
        fails.append("4282: no new query key")

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("PASS check_join_token_observe_4139: all ACs green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
