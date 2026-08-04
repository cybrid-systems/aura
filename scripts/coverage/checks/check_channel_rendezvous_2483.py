#!/usr/bin/env python3
"""Issue #2483: channel:send rendezvous — no buffer_size==0 wait short-circuit.

Contract:
  AC1 waiting_receivers on Channel + send/recv
  AC2 no buggy `buffer_size == 0 || queue.size` Or
  AC3 channels_mtx_ released before wait (ch_ptr pattern)
  AC4 gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_channel_rendezvous.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2483", "AC1", msg)
    must("waiting_receivers", "AC1", ixx)
    must("waiting_receivers", "AC1", msg)

    spos = msg.find("channel:send")
    sbody = msg[spos : spos + 2500] if spos >= 0 else ""
    must_not("ch.buffer_size == 0 || ch.queue.size()", "AC2", sbody)
    must("waiting_receivers", "AC2", sbody)
    must("ch_ptr", "AC3", sbody)

    rpos = msg.find("channel:recv")
    rbody = msg[rpos : rpos + 1500] if rpos >= 0 else ""
    must("waiting_receivers", "AC3-recv", rbody)

    must("2483 AC1", "gate", test)
    must("check_channel_rendezvous_2483", "gate", build)
    must("cmd_channel_rendezvous_coverage", "gate", build)
    must("test_channel_rendezvous", "gate", cmake)
    must("2483 AC4", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: channel rendezvous #2483 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
