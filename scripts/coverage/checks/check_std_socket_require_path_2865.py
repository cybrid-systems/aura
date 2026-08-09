#!/usr/bin/env python3
"""Issue #2865: std/socket must re-export host TCP prims without recursion.

Pass-through wrappers of the form
  (define (tcp-listen port) (tcp-listen port))
shadow host prims after (require "std/socket" all:) and recurse → always ().

Contract (one row per AC):
  AC1 socket.aura uses value aliases (define tcp-listen tcp-listen), not
     recursive procedure wrappers
  AC2 all eight tcp-* names are exported and value-aliased
  AC3 cites #2865; documents no recursive wrappers
  AC4 test_tcp_listen_accept extended with ac2865_*; this linter wired in
     build.py; no docs/design/2865-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

TCP_NAMES = (
    "tcp-connect",
    "tcp-send",
    "tcp-recv",
    "tcp-close",
    "tcp-listen",
    "tcp-local-port",
    "tcp-accept",
    "tcp-accept-timeout",
)


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

    sock = _read("lib/std/socket.aura")
    test = _read("tests/compiler/test_tcp_listen_accept.cpp")
    build = _read("build.py")

    # Strip line comments so documentation of the anti-pattern is not matched.
    sock_code = "\n".join(ln for ln in sock.splitlines() if not ln.lstrip().startswith(";"))

    # AC1 — no recursive procedure wrappers in live code
    for name in TCP_NAMES:
        pat = rf"\(define\s+\({re.escape(name)}\b"
        if re.search(pat, sock_code):
            fails.append(f"AC1: forbidden procedure wrapper for {name}")

    # AC1/AC2 — value aliases present
    for name in TCP_NAMES:
        alias = f"(define {name} {name})"
        if alias not in sock_code:
            fails.append(f"AC2: missing value alias {alias!r}")

    # AC2 — export list includes all names
    for name in TCP_NAMES:
        must(name, "AC2 export", sock)
    must("(export", "AC2 export form", sock_code)

    # AC3 — docs cite
    must("Issue #2865", "AC3", sock)
    if "value alias" not in sock.lower() and "value-alias" not in sock.lower():
        fails.append("AC3: missing value-alias documentation")

    # AC4 — tests + linter wiring
    must("ac2865_1_require_tcp_listen", "AC4", test)
    must("ac2865_2_require_loopback", "AC4", test)
    must("ac2865_3_no_recursive_wrappers", "AC4", test)
    must("ac2865_4_linter", "AC4", test)
    must("Issue #2865", "AC4", test)
    must("check_std_socket_require_path_2865", "AC4", build)
    for rel in (
        "docs/design/2865-std-socket-require.md",
        "docs/2865-std-socket-require.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2865 std/socket require-path re-export — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
