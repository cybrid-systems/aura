#!/usr/bin/env python3
"""Issue #4261: PrimFn drops std::function — FnPtr / SBO trampoline.

PrimFn dual-stored FnPtr + std::function; capturing lambdas paid type-erasure
heap on every apply. Pass pipeline already bans std::function on PureWrap
dirty preds (#3042). Fix: SBO trampoline (+ Soft large-capture heap box once
at registration). finalize_hot_table wraps with PrimFn*+const char* SBO only.

Contract:
  AC1  PrimFn has no std::function member; FnPtr / SBO / uses_std_function APIs
  AC2  Soft capturing registrations still constructible (bind_callable path)
  AC3  finalize_hot_table uses hot_timing_held_ SBO wrap (no PrimFn+string capture)
  AC4  extends test_primitives_hotpath_registry_slo; linter + gf + manifest + build.py
       no invent / docs/design / schema-4261

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
EV = "src/compiler/evaluator.ixx"
TEST = "tests/compiler/test_primitives_hotpath_registry_slo.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/4261.json"
LINTER = "check_primfn_no_std_function_4261"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read(EV)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    must("Issue #4261", "AC1 cite", ev)
    must("kPrimFnNoStdFunctionIssue = 4261", "AC1 stamp", ev)
    must("uses_std_function", "AC1 API", ev)
    must("uses_sbo_trampoline", "AC1 SBO API", ev)
    must("kSboBytes", "AC1 SBO size", ev)
    must("bind_callable", "AC2 Soft bind", ev)

    prim_pos = ev.find("export class PrimFn")
    if prim_pos < 0:
        fails.append("AC1: PrimFn missing")
    else:
        prim_end = ev.find("kPrimFnNoStdFunctionIssue", prim_pos)
        body = ev[prim_pos:prim_end] if prim_end > prim_pos else ev[prim_pos : prim_pos + 4000]
        body_code = re.sub(r"//[^\n]*", "", body)
        if "std::function<" in body_code:
            fails.append("AC1: PrimFn still has std::function member/usage")
        if "fn_" in body_code and "std::function" in body:
            fails.append("AC1: residual fn_ std::function field")

    must("hot_timing_held_", "AC3 finalize wrap", ev)
    must("name_cstr", "AC3 SBO capture", ev)
    # Old capturing wrap must be gone
    if re.search(r"PrimFn timed = \[original\s*,\s*name\]", ev):
        fails.append("AC3: finalize_hot_table still captures PrimFn+string")

    must("4261 AC1", "AC4 test", test)
    must("4261 AC2", "AC4 test Soft", test)
    must("4261 AC3", "AC4 microbench", test)
    must(LINTER, "AC4 build", build)
    must("Issue #4261", "AC4 build cite", build)
    must(LINTER + ".py", "AC4 gf", gf)
    must('"issue": 4261', "AC4 manifest", man)

    if "schema-4261" in ev:
        fails.append("AC4: schema-4261 invent forbidden")
    if (ROOT / "tests/compiler/test_issue_4261.cpp").is_file():
        fails.append("AC4: test_issue_4261.cpp invent forbidden")
    if (ROOT / "docs/design/4261-primfn-no-std-function.md").is_file():
        fails.append("AC4: docs/design invent forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #4261 PrimFn no std::function — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
