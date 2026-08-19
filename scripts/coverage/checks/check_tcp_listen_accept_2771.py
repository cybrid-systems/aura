#!/usr/bin/env python3
"""Issue #2771: tcp-listen / tcp-accept multi-host denseness server path.

std/socket had only client prims (connect/send/recv/close). Hermes Phase 5
could not exercise OS TCP as wire transport E. This lands listen/accept +
local-port + accept-timeout under AURA_ENABLE_TCP.

Contract (one row per AC):
  AC1 prims tcp-listen / tcp-local-port / tcp-accept / tcp-accept-timeout
  AC2 std/socket + adaptive help + loopback docs
  AC3 test_tcp_listen_accept in json_io_cap_batch + ac echo
  AC4 commercial budget tcp- = 8 (static only; no live aura smoke)
  AC5 this linter wired; no docs/design/2771-*

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

    io = _read("src/compiler/evaluator_primitives_io.cpp")
    sock = _read("lib/std/socket.aura")
    adaptive = _read("lib/std/adaptive.aura")
    t = _read("tests/compiler/test_tcp_listen_accept.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    surface = _read("scripts/coverage/checks/check_primitive_surface.py")
    tc = _read("src/compiler/type_checker_impl.cpp")

    # AC1
    for name in (
        "tcp-listen",
        "tcp-local-port",
        "tcp-accept",
        "tcp-accept-timeout",
    ):
        must(f'defer_std_host_prim("{name}"', "AC1", io)
        must(name, "AC1 type_checker", tc)
    must("#2771", "AC1", io)
    must("INADDR_LOOPBACK", "AC1", io)

    # AC2
    must("tcp-listen", "AC2", sock)
    must("tcp-accept", "AC2", sock)
    must("#2771", "AC2", sock)
    must("tcp-listen", "AC2", adaptive)
    must("#2771", "AC2", adaptive)

    # AC3
    must("ac2_ac3_echo_fiber_client", "AC3", t)
    must("echo:ping", "AC3", t)
    must("fiber:spawn", "AC3", t)
    must("test_tcp_listen_accept.cpp", "AC3", cmake)
    must("run_test_tcp_listen_accept", "AC3", _read("tests/compiler/test_json_io_cap_batch.cpp"))

    # AC4
    must('tcp-": 8', "AC4", surface)
    must("#2771", "AC4", surface)

    # AC5
    must("check_tcp_listen_accept_2771", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2771-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2771.cpp").is_file():
        fails.append("AC5: test_issue_2771.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2771 tcp-listen/accept multi-host denseness — prims + std/socket")
    return 0


if __name__ == "__main__":
    sys.exit(main())
