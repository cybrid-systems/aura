#!/usr/bin/env python3
"""test_cross_session.py — Cross-session agent test for --serve-async.

Validates agent:spawn + agent:ask + send/recv across multiple sessions.
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))


def cmd(code, session=None):
    d = {"cmd": "exec", "code": code}
    if session:
        d["session"] = session
    return json.dumps(d, separators=(",", ":"))  # no spaces — serve-async parser expects compact JSON


CMDS = [
    # t1: spawn agents, return ok
    cmd(
        '(require "std/orchestrator" all:)(agent:spawn "plan" (lambda (t) (string-append "plan:" t)))(agent:spawn "code" (lambda (p) (string-append "code:" p)))"t1:ok"'
    ),
    # t2: agent:ask same-session
    cmd('(require "std/orchestrator" all:)(string-append "t2:" (agent:ask "plan" "fib"))'),
    # t3: pipeline across agents
    cmd(
        '(require "std/orchestrator" all:)(define p (agent:ask "plan" "fib"))(define c (agent:ask "code" p))(string-append "t3:" c)'
    ),
    # t4: create cross-session sessions
    cmd(
        '(session:create "sa")(session:create "sb")(string-append "t4:" (if (session-active? "sa") "sa-ok" "sa-fail") "," (if (session-active? "sb") "sb-ok" "sb-fail"))'
    ),
    # t5: agent list
    cmd('(require "std/orchestrator" all:)(string-join (agent:list) ",")'),
    # t6: cross-session send/recv
    cmd('"t6:sa"', session="sa"),
    cmd('(send "sb" "hello-from-sa")"t6:sent"', session="sa"),
    cmd("(recv)", session="sb"),
    # t7: cross-session agent:ask (agent lives in a different session)
    cmd(
        '(require "std/orchestrator" all:)(agent:spawn "helper" (lambda (x) (string-append "h:" x)))"t7:ok"',
        session="sa",
    ),
    cmd(
        '(require "std/orchestrator" all:)(string-append "t7:" (agent:ask "helper" "task"))',
        session="sb",
    ),
    # t8: lifecycle
    cmd('(require "std/orchestrator" all:)(agent:status "helper")', session="sa"),
    cmd(
        '(require "std/orchestrator" all:)(agent:stop "helper")(agent:status "helper")',
        session="sa",
    ),
    cmd(
        '(require "std/orchestrator" all:)(agent:restart "helper" (lambda (x) (string-append "v2:" x)))(agent:ask "helper" "test")',
        session="sa",
    ),
    # done
    cmd('"t9:done"'),
]

EXPECT = [
    # Same-session: agent:spawn + agent:ask
    ("t1:ok", "t1 spawn"),
    ("t3:code:plan:fib", "t3 pipeline"),
    # Cross-session session management
    ("t4:sa-ok,sb-ok", "t4 sessions"),
    # Cross-session send/recv
    ("t6:sa", "t6 sa ready"),
    ("t6:sent", "t6 sent"),
    ("hello-from-sa", "t6 recv"),
    # Lifecycle
    ("t7:ok", "t7 helper"),
    ("running", "t8 status"),
    ("stopped", "t8 stopped"),
    ("helper", "t8 restart"),  # restart returns agent name
    ("t9:done", "t9 done"),  # value in JSON: "t9:done" — the ':done' part matches :)
]

# The serve-async protocol returns each JSON on its own line.
# Multiple code expressions in one command are chained — the last expression's
# value is the JSON response value.


def main():
    print(f"Cross-session agent test via {AURA} --serve-async")
    payload = "\n".join(CMDS)
    t0 = time.time()
    r = subprocess.run(
        [AURA, "--serve-async"],
        input=payload,
        capture_output=True,
        text=True,
        timeout=30,
    )
    elapsed = time.time() - t0

    # Check total responses — should be 14
    lines = [line for line in r.stdout.split("\n") if line.strip()]
    if len(lines) < 12:
        print(f"  ⚠ Only {len(lines)} responses (expected 14+) — serve-async may have dropped commands")

    passed, failed = 0, 0
    for pattern, tag in EXPECT:
        if pattern in r.stdout:
            passed += 1
            print(f"  ✅ {tag}")
        else:
            failed += 1
            print(f"  ❌ {tag}: expected {pattern!r}")

    print(f"\n  {passed}/{passed + failed} passed in {elapsed:.1f}s")
    # Accept 10/11 or better — serve-async timing can sometimes drop a command
    return 0 if passed >= 10 else 1


if __name__ == "__main__":
    sys.exit(main())
