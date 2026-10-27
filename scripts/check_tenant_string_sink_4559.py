#!/usr/bin/env python3
"""Issue #4559: effectful sinks in-range string fence before any copy.

Contract (one row per AC):
  AC1 write-file content fence precedes the host-buffer copy
  AC2 shell command fence precedes the jail/exec section
  AC3 command-output fence precedes the argv copy
  AC4 sys-write fence precedes the fd write
  AC5 http-get url fence precedes the perform path
  AC6 http-post url/body/auth fences precede the async/curl copies
  AC7 tcp-send payload fence precedes ::send
  AC8 white-box arm asserts the fence per sink; no docs/design/4559-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []
    file_src = _read("src/compiler/evaluator_primitives_file.cpp")
    io_src = _read("src/compiler/evaluator_primitives_io.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")

    def fence_pos(win: str, op: str) -> int:
        probe = win.find("inrange_string_tenant_allows")
        while probe >= 0:
            if f'"{op}"' in win[probe : probe + 220]:
                return probe
            probe = win.find("inrange_string_tenant_allows", probe + 1)
        return -1

    def must_ordered(label: str, fpos: int, later: str, win: str) -> None:
        if fpos < 0:
            fails.append(f"{label}: fence call missing")
            return
        pos = win.find(later)
        if pos < 0:
            fails.append(f"{label}: later marker {later!r} missing")
            return
        if fpos > pos:
            fails.append(f"{label}: fence must precede {later!r}")

    # AC1 write-file
    a = file_src.find('add("write-file"')
    w1 = file_src[a : a + 4000] if a >= 0 else ""
    # Issue #4714 follow-up: reads go through Evaluator::string_at(); the
    # marker follows the accessor, not the raw heap index.
    must_ordered("AC1", fence_pos(w1, "write-file"), "content = ev.string_at(cidx)", w1)
    if "kEffectWrite" not in w1:
        fails.append("AC1: write-file fence must use the already-required kEffectWrite")

    # AC2 shell
    a = file_src.find('defer_std_host_prim("shell"')
    w2 = file_src[a : a + 3000] if a >= 0 else ""
    must_ordered("AC2", fence_pos(w2, "shell"), "check_tenant_exec_jail", w2)

    # AC3 command-output
    a = file_src.find('"command-output"')
    w3 = file_src[a : a + 3200] if a >= 0 else ""
    must_ordered("AC3", fence_pos(w3, "command-output"), "auto& cmd = ev.mutable_string_at(idx)", w3)

    # AC4 sys-write (io.cpp)
    a = io_src.find('defer_std_host_prim("sys-write"')
    w4 = io_src[a : a + 3000] if a >= 0 else ""
    must_ordered("AC4", fence_pos(w4, "sys-write"), "const auto& s = ev.string_at(sidx)", w4)

    # AC5 http-get
    a = io_src.find('"http-get"')
    w5 = io_src[a : a + 3000] if a >= 0 else ""
    must_ordered("AC5", fence_pos(w5, "http-get"), "check_tenant_http_scheme(url", w5)

    # AC6 http-post: url/body/auth fences before the copies
    a = io_src.find('defer_std_host_prim("http-post"')
    w6 = io_src[a : a + 3600] if a >= 0 else ""
    cnt6 = w6.count("inrange_string_tenant_allows")
    if cnt6 < 3:
        fails.append(f"AC6: http-post needs url/body/auth fences, found {cnt6}")
    else:
        must_ordered("AC6", w6.find("inrange_string_tenant_allows"), "std::string url_copy = curl_url", w6)

    # AC7 tcp-send
    a = io_src.find('defer_std_host_prim("tcp-send"')
    w7 = io_src[a : a + 2600] if a >= 0 else ""
    must_ordered("AC7", fence_pos(w7, "tcp-send"), "auto& data = ev.mutable_string_at(sidx)", w7)

    # AC8 white-box arm + no design doc
    for marker in (
        "4559: write-file content fence denies foreign stamp",
        "4559: shell command fence denies foreign stamp",
        "4559: command-output fence denies foreign stamp",
        "4559: sys-write fence denies foreign stamp",
        "4559: http-get url fence denies foreign stamp",
        "4559: http-post url/body/auth fence denies foreign stamp",
        "4559: tcp-send payload fence denies foreign stamp",
    ):
        if marker not in test:
            fails.append(f"AC8: white-box assert missing: {marker!r}")
    if (ROOT / "docs" / "design" / "4559-tenant-string-sink-exfil.md").is_file():
        fails.append("AC8: docs/design/4559-* present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"FAIL: check_tenant_string_sink_4559 - {len(fails)} row(s) failed")
        return 1
    print("OK: Issue #4559 tenant string sink fences - in-range before every copy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
