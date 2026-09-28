#!/usr/bin/env python3
"""Issue #4137: production C++ agent_recv steal×held_ref face (steal arm
of the #3336 / #4001 send/recv preference family).

After fiber steal, pending held_ref messages are revalidated and
handoff_completed cleared (maybe_revalidate_held_ref_after_attach,
#3942 attach hook + #3967 seq guard). agent_recv_result under
production returns typed "handoff-required" (Aura orch:agent-recv
surfaces the typed deny); raw agent_recv returns only .message →
nullopt, indistinguishable from quiet empty for hosts that do not read
last_recv_stale_handoff / held_ref_stale_after_steal_total. The
dual-track must stay observable: document the coincidence on the raw
wrapper, keep the steal soak typed, and never surface silent Closed for
a handoff miss.

Contract (one row per AC):
  AC1  steal×handoff soak is typed: agent_spawn.h steal arm returns
       prod ? "handoff-required" : "empty"; stamp
       kRecvStealHandoffIssue = 4137; production src/ raw agent_recv(
       call sites are typed fall-through / definitions / annotated
       orch-raw-recv-ok; test ac4137_1_steal_soak_handoff_required_and_counters
  AC2  raw nullopt ≡ last_recv_stale_handoff documented on the raw
       wrapper (Issue #4137 cite); revalidation hook present; test
       ac4137_2_raw_nullopt_coincides_with_stale_flag
  AC3  counters held_ref_post_steal_check_total /
       held_ref_stale_after_steal_total reused (no invented counter);
       RecvResult never surfaces "closed" for a handoff miss; test
       ac4137_3_bp_storm_held_ref_stays_typed
  AC4  linter wired in build.py after the #4001 Guard-face twin; source
       + linter row; no test_issue_4137.cpp; no docs/design/4137-*; no
       new hash schema key

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "src"
CALL_RE = re.compile(r"(?<![\w])agent_recv\s*\(")
OK_TAG = "orch-raw-recv-ok"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _is_allowed_call(code: str, full_line: str, prev_full: str) -> bool:
    if OK_TAG in full_line or OK_TAG in prev_full:
        return True
    stripped = code.strip()
    if "RecvResult" in stripped and "agent_recv" in stripped:
        return True  # definition / forward decl with return type on this line
    if stripped.startswith("agent_recv(") and ("optional<" in prev_full or "RecvResult" in prev_full):
        return True  # wrapped definition: signature line, return type above
    return stripped.startswith("return agent_recv")


def _scan_production_agent_recv() -> list[str]:
    fails: list[str] = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in {".h", ".hh", ".cpp", ".ixx"}:
            continue
        rel = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        prev_full = ""
        for i, line in enumerate(text.splitlines(), 1):
            full = line
            code = line.split("//", 1)[0]
            if CALL_RE.search(code) and not _is_allowed_call(code, full, prev_full):
                fails.append(f"AC1: {rel}:{i} raw agent_recv without agent_recv_result / {OK_TAG}")
            if line.strip():
                prev_full = full
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    spawn = _read("src/orch/agent_spawn.h")
    mailbox = _read("src/serve/multi_fiber_mailbox.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_obs_facade.cpp")
    twin4001 = _read("scripts/coverage/checks/check_agent_recv_typed_preference_4001.py")
    build = _read("build.py")

    # AC1 — steal soak typed + production sites typed or annotated.
    fails.extend(_scan_production_agent_recv())
    must("kRecvStealHandoffIssue = 4137", "AC1 stamp", spawn)
    must('prod ? "handoff-required" : "empty"', "AC1 steal arm", spawn)
    must("ac4137_1_steal_soak_handoff_required_and_counters", "AC1 test", test)

    # AC2 — raw nullopt ≡ last_recv_stale_handoff documented.
    must("Issue #4137: post-steal stale held_ref nullopt coincides", "AC2 raw wrapper doc", spawn)
    must("last_recv_stale_handoff", "AC2 handle flag", spawn)
    must("maybe_revalidate_held_ref_after_attach", "AC2 revalidation hook", spawn)
    must("ac4137_2_raw_nullopt_coincides_with_stale_flag", "AC2 test", test)

    # AC3 — counters reused; no silent Closed on the handoff miss.
    must("held_ref_post_steal_check_total", "AC3 post-steal counter", mailbox)
    must("held_ref_stale_after_steal_total", "AC3 stale counter", mailbox)
    must("ac4137_3_bp_storm_held_ref_stays_typed", "AC3 test", test)
    rr = spawn.find("struct RecvResult")
    ar = spawn.find("agent_recv_result(")
    if rr < 0 or ar < 0 or rr > ar:
        fails.append("AC3: RecvResult struct / agent_recv_result SSOT missing")
    elif "closed" in spawn[rr:ar].lower():
        fails.append("AC3: RecvResult must not surface Closed (handoff miss stays handoff-required)")

    # AC4 — wiring + no invent.
    must("ac4137_4_source_and_linter", "AC4 test", test)
    must("check_agent_recv_steal_handoff_4137", "AC4 build.py", build)
    must("Issue #4001", "AC4 #4001 Guard-face twin retained", twin4001)
    prev = build.find("check_agent_recv_typed_preference_4001")
    ours = build.find("check_agent_recv_steal_handoff_4137")
    if ours < 0:
        fails.append("AC4: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC4: steal linter must be wired in build.py AFTER the #4001 Guard-face twin")
    if (ROOT / "tests" / "orch" / "test_issue_4137.cpp").is_file():
        fails.append("AC4: tests/orch/test_issue_4137.cpp present (forbidden #81934)")
    if "schema-4137" in prim:
        fails.append("AC4: new hash schema key added (Aura keys must stay unchanged)")
    if "agent_recv_raw" in spawn:
        fails.append("AC4: invented recv_raw counter present (#4137 reuses existing keys)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4137-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4137 agent_recv steal×held_ref handoff — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
