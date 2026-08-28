#!/usr/bin/env python3
"""Issue #3336: production C++ agent_send preference after #3013/#3212.

Raw agent_send remains for zero-cost non-held_ref / already-stamped.
New non-test production TUs must use agent_send_safe or annotate
`// orch-raw-send-ok`. Unstamped held_ref still returns HandoffRequired.

Contract (one row per AC):
  AC1  production src/ agent_send( call sites are agent_send_safe fall-through,
       definitions, or annotated orch-raw-send-ok
  AC2  zero-cost no-held_ref / already-stamped path unchanged
  AC3  Soft/Off quiet path: no new atomic (raw_held_ref counter only on
       unstamped held_ref)
  AC4  unstamped held_ref still HandoffRequired (#3013/#3212 regression)
  AC5  extend test_orch_obs_facade; linter after #3013/#3212;
       no invent / no docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "src"
CALL_RE = re.compile(r"(?<![\w])agent_send\s*\(")
OK_TAG = "orch-raw-send-ok"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _is_allowed_call(code: str, full_line: str, prev_full: str) -> bool:
    if OK_TAG in full_line or OK_TAG in prev_full:
        return True
    stripped = code.strip()
    if "PushStatus" in stripped and "agent_send" in stripped:
        return True  # definition / forward decl
    return stripped.startswith("return agent_send")


def _scan_production_agent_send() -> list[str]:
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
                fails.append(f"AC1: {rel}:{i} raw agent_send without agent_send_safe / {OK_TAG}")
            if line.strip():
                prev_full = full
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_obs_facade.cpp")
    lint3013 = _read("scripts/coverage/checks/check_agent_send_handoff_required_3013.py")
    lint3212 = _read("scripts/coverage/checks/check_mailbox_handoff_dual_track_3212.py")
    build = _read("build.py")

    fails.extend(_scan_production_agent_send())
    must("kAgentSendSafePreferenceIssue = 3336", "AC1 stamp", spawn)
    must(OK_TAG, "AC1 language-path annotation", prim)
    must("ac3336_1_production_sites_safe_or_annotated", "AC1 test", test)

    must("Zero-cost when no held_ref_token", "AC2 quiet path", spawn)
    must("ac3336_2_zero_cost_plain_unchanged", "AC2 test", test)

    must("agent_send_raw_held_ref_total{0}", "AC3 counter at END", spawn)
    dtor_pos = spawn.find("reclaimed_abandon_total{0}")
    raw_pos = spawn.find("agent_send_raw_held_ref_total{0}")
    if raw_pos < 0 or dtor_pos < 0 or raw_pos < dtor_pos:
        fails.append("AC3: agent_send_raw_held_ref_total must END-append after reclaimed_abandon")
    send = spawn.find("Issue #3013: unstamped held_ref is a typed handoff miss")
    if send < 0:
        fails.append("AC3: agent_send #3013 gate missing")
    else:
        body = spawn[send : send + 1800]
        bump = body.find("agent_send_raw_held_ref_total.fetch_add")
        quiet = body.find("producer_throttled")
        if bump < 0:
            fails.append("AC3: raw_held_ref bump missing on unstamped path")
        elif quiet > 0 and bump > quiet:
            fails.append("AC3: raw_held_ref bump must be on unstamped held_ref, not quiet path")
    must("ac3336_3_soft_quiet_no_raw_counter", "AC3 test", test)

    must("HandoffRequired", "AC4", spawn)
    must("ac3336_4_unstamped_still_handoff_required", "AC4 test", test)
    must("Issue #3013", "AC4 #3013 linter retained", lint3013)
    must("Issue #3212", "AC4 #3212 linter retained", lint3212)

    must("check_agent_send_safe_preference_3336", "AC5 build.py", build)
    must("check_agent_send_handoff_required_3013", "AC5 #3013 wired", build)
    must("check_mailbox_handoff_dual_track_3212", "AC5 #3212 wired", build)
    must("ac3336_5_source_and_linter", "AC5 test", test)
    must("schema-3336", "AC5 schema", prim)
    must("agent-send-raw-held-ref-total", "AC5 stats key", prim)
    prev = build.find("check_mailbox_handoff_dual_track_3212")
    ours = build.find("check_agent_send_safe_preference_3336")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3212")
    if (ROOT / "tests" / "orch" / "test_issue_3336.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3336.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3336-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3336 agent_send_safe preference — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
