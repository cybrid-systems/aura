#!/usr/bin/env python3
"""Issue #3642: stale held_ref consumes as nullopt (C++ recv double-track).

Contract (one row per AC):
  AC1  MultiFiberMailbox::recv / try_pop consume a stale held_ref
       (production, token set, !handoff_completed) as nullopt / false —
       the cleared payload never surfaces as a successful delivery;
       the discarded-return `(void)maybe_clear...` consume sites are gone
  AC2  the stale signal rides AgentHandle.last_recv_stale_handoff so the
       Aura orch:agent-recv typed handoff-required surface (#3565 AC2)
       still fires; Soft still delivers (#3111 AC3); no new query key
  AC3  ACs live in tests/orch/test_orch_obs_facade.cpp (3642 markers:
       raw recv nullopt / try_pop false / handle flag); the #3565 AC1
       has_value assertion is updated to the nullopt contract
  AC4  no tests/**/test_issue_3642.cpp; no docs/design/3642-*
  AC5  build.py wires check_recv_stale_nullopt_3642 + root allowlist

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

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    mb = _read("src/serve/multi_fiber_mailbox.h")
    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_obs_facade.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: mailbox consume paths fail closed on stale ──────────────────
    must("Issue #3642", "AC1 mailbox cites", mb)
    # Format-robust: locate the stale_handoff parameter, then walk back to
    # the recv definition (clang-format may reflow the signature lines).
    sig = mb.find("bool* stale_handoff)")
    rstart = mb.rfind("std::optional<MailMessage> recv(", 0, sig) if sig >= 0 else -1
    if sig < 0 or rstart < 0:
        fails.append("AC1: 4-arg recv overload missing")
    else:
        window = mb[rstart : sig + 3600]
        if "if (maybe_clear_stale_held_ref_on_recv(out, &local_stats_)) {" not in window:
            fails.append("AC1: recv stale branch missing")
        if "*stale_handoff = true;" not in window:
            fails.append("AC1: stale flag not reported")
        if "return std::nullopt;" not in window:
            fails.append("AC1: recv does not return nullopt on stale")
    tp = mb.find("[[nodiscard]] bool try_pop(MailMessage& out)")
    if tp < 0:
        fails.append("AC1: try_pop missing")
    else:
        tpw = mb[tp : tp + 900]
        if "if (maybe_clear_stale_held_ref_on_recv(out, &local_stats_))\n            return false;" not in tpw:
            fails.append("AC1: try_pop does not return false on stale")
    forbid("(void)maybe_clear_stale_held_ref_on_recv", "AC1 discarded-return gone", mb)

    # ── AC2: stale signal rides the handle; Aura surface unchanged ───────
    must("bool last_recv_stale_handoff = false", "AC2 handle flag field", spawn)
    must("Issue #3642", "AC2 agent_recv cites", spawn)
    aidx = spawn.find("bool stale_handoff = false;")
    if aidx < 0:
        fails.append("AC2: agent_recv stale plumbing missing")
    else:
        aw = spawn[aidx : aidx + 1400]
        if "h.mailbox->recv(wait, timeout_ms, h.id, &stale_handoff)" not in aw:
            fails.append("AC2: agent_recv not passing stale out-param")
        if "h.last_recv_stale_handoff = true;" not in aw:
            fails.append("AC2: handle flag not set on stale")
        if "return std::nullopt;" not in aw:
            fails.append("AC2: agent_recv does not return nullopt on stale")
    must("hp->last_recv_stale_handoff", "AC2 primitive rides flag", prim)
    must("stale_handoff_surface", "AC2 primitive stale condition", prim)
    must("handoff-required", "AC2 typed surface unchanged", prim)
    must("kRecvHeldRefAfterStealIssue", "AC2 #3565 constant retained", prim)

    # ── AC3: test placement + updated contract ───────────────────────────
    for n in (
        "3642 AC1: raw mailbox recv is nullopt on stale",
        "3642 AC1: try_pop on stale returns false",
        "3642 AC1: stale recv is nullopt (not a success)",
        "3642 AC1: later message pops after stale consume",
        "3642 AC2: handle flag set for Aura typed fail",
        "3642 AC6: build.py wires linter",
    ):
        must(n, "AC3 test marker", test)
    forbid("3565 AC1: popped (queue not stuck)", "AC3 old has_value contract gone", test)

    # ── AC4: no new files in forbidden slots ─────────────────────────────
    if _read("tests/orch/test_issue_3642.cpp"):
        fails.append("AC4: tests/orch/test_issue_3642.cpp exists")
    if _read("tests/issues/test_issue_3642.cpp"):
        fails.append("AC4: tests/issues/test_issue_3642.cpp exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in sorted(design.glob("3642-*")):
            fails.append(f"AC4: docs/design file present: {p.name}")

    # ── AC5: gate wiring ─────────────────────────────────────────────────
    must("check_recv_stale_nullopt_3642", "AC5 build.py wires linter", build)
    must("check_recv_stale_nullopt_3642.py", "AC5 root allowlist", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("3642 recv stale nullopt: all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
