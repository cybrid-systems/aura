#!/usr/bin/env python3
"""Issue #4001 / #4116: production C++ agent_recv preference (Guard-live
nullopt≡empty dual-track).

Raw agent_recv returns only optional<MailMessage> — production Guard-live
Policy A empty becomes nullopt, indistinguishable from quiet empty, so C++
hosts can busy-wait under MutationBoundary. New non-test production TUs
must use agent_recv_result / agent_recv_safe or annotate
`// orch-raw-recv-ok`. The raw wrapper stays payload-or-empty; typed
status (recv-under-boundary / handoff-required) is the SSOT.

Contract (one row per AC):
  AC1  production src/ agent_recv( call sites are agent_recv_result /
       agent_recv_safe fall-through, definitions, or annotated
       orch-raw-recv-ok; stamp kRecvTypedStatusIssue = 4001; Aura path
       uses agent_recv_result
  AC2  Soft quiet empty unchanged (recv_empty_total, status "empty");
       Aura orch:agent-recv hash keys unchanged (ok / empty / payload)
  AC3  Guard-live typed status remains recv-under-boundary (not Closed)
  AC4  no new OrchModuleStats mid-schema keys; recv path reuses
       recv_empty_total / agents_recv
  AC5  linter wired in build.py after the #3336 send twin; extends
       test_orch_obs_facade.cpp (#81967); no invent / no docs/design

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
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_obs_facade.cpp")
    lint3336 = _read("scripts/coverage/checks/check_agent_send_safe_preference_3336.py")
    build = _read("build.py")

    fails.extend(_scan_production_agent_recv())
    must("kRecvTypedStatusIssue = 4001", "AC1 stamp", spawn)
    must("agent_recv_result(*hp, wait, timeout_ms)", "AC1 language path", prim)
    must("ac4116_1_production_sites_typed_or_annotated", "AC1 test", test)

    must('out.status = "empty";', "AC2 quiet empty status", spawn)
    must("recv_empty_total.fetch_add", "AC2 quiet empty counter", spawn)
    must('{"ok", make_bool(false)},', "AC2 hash ok key", prim)
    must('{"empty", make_bool(true)},', "AC2 hash empty key", prim)
    must('{"ok", make_bool(true)},', "AC2 hash delivery key", prim)
    must("ac4116_2_soft_quiet_empty_unchanged", "AC2 test", test)

    must('"recv-under-boundary"', "AC3 Guard-live status", spawn)
    must("Issue #4001: typed C++ status", "AC3 typed arm", spawn)
    must("ac4116_3_guard_live_recv_under_boundary", "AC3 test", test)
    rr = spawn.find("struct RecvResult")
    ar = spawn.find("agent_recv_result(")
    if rr < 0 or ar < 0 or rr > ar:
        fails.append("AC3: RecvResult struct / agent_recv_result SSOT missing")
    elif "closed" in spawn[rr:ar].lower():
        fails.append("AC3: RecvResult must not surface Closed (Guard-live stays recv-under-boundary)")

    if "agent_recv_raw" in spawn:
        fails.append("AC4: invented recv_raw counter present (#4001 reuses existing keys)")
    must("recv_empty_total", "AC4 reuse recv_empty_total", spawn)
    must("agents_recv", "AC4 reuse agents_recv", spawn)
    if "schema-4001" in prim:
        fails.append("AC4: new hash schema key added (Aura keys must stay unchanged)")
    must("ac4116_4_reuses_existing_recv_counters", "AC4 test", test)

    must("check_agent_recv_typed_preference_4001", "AC5 build.py", build)
    must("ac4116_5_source_and_linter", "AC5 test", test)
    must("Issue #3336", "AC5 #3336 send twin retained", lint3336)
    prev = build.find("check_agent_send_safe_preference_3336")
    ours = build.find("check_agent_recv_typed_preference_4001")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: recv linter must be wired in build.py AFTER the #3336 send twin")
    if (ROOT / "tests" / "orch" / "test_issue_4116.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_4116.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4116-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4001/#4116 agent_recv typed preference — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
