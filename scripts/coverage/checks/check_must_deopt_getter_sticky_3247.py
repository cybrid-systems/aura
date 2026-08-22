#!/usr/bin/env python3
"""Issue #3247: aura_get_closure_must_deopt_before_next_call is sticky observe.

Comment claimed clear-on-read; impl is read-only. Option A: keep sticky
fail-closed until remount/remap/alloc/aura_closure_call force-deopt.
Agent must not treat the getter as consume.

Contract (one row per AC):
  AC1  comment/header match impl (does not clear; shared-lock read)
  AC2  aura_closure_call still exclusive-clears + #2472 + poison epoch
  AC3  suite: N getter probes stay 1; heal → 0; free+realloc no consume
  AC4  no new metrics keys; Soft/Off unchanged (existing C ABI)
  AC5  extend test_must_deopt_before_next_call; no invent; no docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    hdr = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_must_deopt_before_next_call.cpp") + _read(
        "tests/compiler/test_closure_call_must_deopt_toctou.cpp"
    )
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    g = rt.find('extern "C" int aura_get_closure_must_deopt_before_next_call')
    getter = rt[max(0, g - 500) : g + 550] if g >= 0 else ""
    fn = rt[g : g + 450] if g >= 0 else ""
    must("aura_get_closure_must_deopt_before_next_call", "AC1 getter", rt)
    must("does **not** clear", "AC1 comment", getter)
    must("Agent", "AC1 Agent not consume", getter)
    must("shared_lock", "AC1 shared-lock read", fn)
    must_not("clears it", "AC1 old clear-on-read claim", getter)
    must_not("g_closure_must_deopt[cid] = 0", "AC1 getter stores 0", fn)
    must_not("fetch_add", "AC4 no new metric in getter", fn)
    must("aura_get_closure_must_deopt_before_next_call", "AC1 header", hdr)
    must("does **not** clear", "AC1 header sticky", hdr)
    must("Agent must not treat", "AC1 header Agent", hdr)

    call = rt.find("int64_t aura_closure_call(")
    body = rt[call : call + 4500] if call >= 0 else ""
    must("g_closure_must_deopt[cid] = 0", "AC2 call still clears", body)
    must("g_closure_bridge_epochs[cid] = 0", "AC2 poison epoch", body)
    must("Issue #2472", "AC2 identity recheck", body)
    must("Issue #3247", "AC2 getter is not consume", body)
    must("unique_lock", "AC2 exclusive clear", body)

    must("ac3247_1_sticky", "AC3 sticky test", test)
    must("ac3247_2_heal", "AC3 heal test", test)
    must("ac3247_2_call", "AC3 call still consumes", test)
    must("ac3247_3_realloc", "AC3 free+realloc", test)
    must("aura_get_closure_must_deopt_before_next_call", "AC3 getter used", test)

    must("aura_get_closure_must_deopt_before_next_call", "AC4 stub", stub)
    must("check_must_deopt_getter_sticky_3247", "AC5 build.py", build)
    must("cmd_must_deopt_getter_sticky_3247_coverage", "AC5 cmd", build)
    must("test_must_deopt_before_next_call", "AC5 cmake", cmake)
    if _read("tests/compiler/test_issue_3247.cpp") or _read("tests/issues/test_issue_3247.cpp"):
        fails.append("AC5: test_issue_3247.cpp present (forbidden #81967)")
    if _read("docs/design/3247-must-deopt-getter.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3247 must_deopt_getter_sticky:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3247 must_deopt_getter_sticky: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
