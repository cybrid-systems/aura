#!/usr/bin/env python3
"""Issue #4606 source-cite gate: --jit primitive results came back as raw
words (string?/eqv?/memv/getenv/fiber:join/channel:recv/monotonic-ms).

Root cause: JIT'd OpCall lowers to aura_closure_call ->
aura_closure_dispatch_native_checked. An evaluator-level primitive loaded
from a global/define cell is a RefPrimitive-tagged word —
(slot << kRefPoolShift) | (RefPrimitive << kRefTypeShift) | kRefLowTag
(3733 = string? slot 58, 23765 = getenv slot 371, per the issue repro).
The dispatcher's bounds-fail fallthrough echoed that word back to the
JIT'd caller ("return callee_val for non-callable"), so every --jit branch
on such a prim result read a prim reference instead of #f and took the
wrong arm. eval and AURA_FORCE_IR=1 were correct: the IR executor's OpCall
has an is_primitive arm (#1676/#3834: invoke_prim_with_telemetry through
context_.primitives) and #4478 turned non-callables into a raise — the
stale echo contract predates both.

Fix: the blessed dispatch entry answers a RefPrimitive-tagged callee word
with the established leave-native refuse (closure_dispatch_leave_native:
#4084 prologue sentinel + fixnum 0, the freed-closure arm's shape). The
#4451/#4084 driver protocol then returns nullopt and re-runs the restored
IR, where the interpreter's is_primitive OpCall arm executes the identical
call — the result is eval-identical by construction. Workspace read lock
is dropped before arming: the resumed interpreter runs arbitrary evaluator
code under its own locks, none of which may nest inside
g_closure_table_mtx.

ACs:
  AC1  aura_jit_runtime.cpp: inside the blessed
       aura_closure_dispatch_native_checked entry, the #4606 arm tests the
       callee word with the value_tags SSOT (types::is_ref +
       types::RefPrimitive), answers with closure_dispatch_leave_native(),
       sits AFTER the workspace-read unlock and BEFORE the legacy
       "match IRInterpreter" callee_val echo (negative + non-prim words
       keep the echo).
  AC2  aura_jit_runtime.cpp: the arm documents the raw-word root cause
       (#4606), the stale-contract ordering (#1676/#3834/#4478), and the
       deopt-to-interpreter parity contract; the refuse reuses the
       #4451/#4084 sentinel currency instead of a second deopt model.
  AC3  tests/compiler/test_closure_call_must_deopt_toctou.cpp: ac4606_
       face registered in run_test_closure_call_must_deopt_toctou — the
       issue's raw words decode as make_ref(RefPrimitive, 58/371), the
       entry refuses a prim word with 0 + armed sentinel + take-consumes,
       non-prim ref words keep the echo; source-cite rows pin the arm and
       its ordering. No tests/**/test_issue_4606*.cpp, no
       docs/design/4606-* (per #81934/#1655).
  AC4  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
TST = ROOT / "tests" / "compiler" / "test_closure_call_must_deopt_toctou.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, row: str) -> None:
    if not cond:
        failures.append(row)


def window(src: str, start: str, size: int) -> tuple[int, str]:
    """Locate `start` and return (index, window) with -1/"" when absent."""
    at = src.find(start)
    if at < 0:
        return -1, ""
    return at, src[at : at + size]


def main() -> int:
    rt = RT.read_text() if RT.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOW.read_text() if ALLOW.exists() else ""

    # ── AC1: the refuse arm inside the blessed entry ───────────────
    must("Issue #4606" in rt, "AC1: runtime TU cites #4606")
    at, body = window(rt, "int64_t aura_closure_dispatch_native_checked(", 3200)
    must(at >= 0, "AC1: blessed dispatch entry present")
    if at >= 0:
        arm = body.find("Issue #4606")
        must(arm > 0, "AC1: #4606 arm inside the entry body")
        must("aura::compiler::types::is_ref(closure_id)" in body, "AC1: callee word tested with types::is_ref")
        must("aura::compiler::types::RefPrimitive" in body, "AC1: RefPrimitive tag compare")
        must("closure_dispatch_leave_native()" in body, "AC1: refuse reuses the leave-native shape")
        unlock = body.find("aura_unlock_workspace_read();")
        must(unlock >= 0 and arm > 0 and unlock < arm, "AC1: workspace read dropped before the arm")
        echo = body.find("return closure_id; /* match IRInterpreter")
        must(echo >= 0 and arm < echo, "AC1: refuse arm precedes the legacy callee_val echo")

    # ── AC2: contract documentation, no second deopt model ─────────
    must("kRefLowTag" in rt or "RefPrimitive" in rt, "AC2: word encoding documented against the value_tags SSOT")
    must("#1676" in rt and "#4478" in rt, "AC2: stale-contract ordering cited (#1676/#3834 primitive arm, #4478 raise)")
    must("#4084" in rt and "#4451" in rt, "AC2: refuse rides the existing #4084/#4451 sentinel currency")
    must("is_primitive arm" in rt, "AC2: deopt-to-interpreter parity noted (interpreter is_primitive arm)")

    # ── AC2b: the driver-level parity fallback ───────────────────
    svc = ROOT / "src" / "compiler" / "service.ixx"
    svc_src = svc.read_text() if svc.exists() else ""
    must("Issue #4606" in svc_src, "AC2: driver TU cites #4606")
    svc_at = svc_src.find("[[nodiscard]] EvalResult exec_jit(")
    must(svc_at >= 0, "AC2: exec_jit entry present")
    if svc_at >= 0:
        sent = svc_src.find("aura_jit_take_prologue_deopt_sentinel() != 0", svc_at)
        must(sent >= 0, "AC2: exec_jit reads the #4084 sentinel")
        fb = svc_src.find("return eval_ir(input);", svc_at)
        must(fb >= 0, "AC2: exec_jit re-runs the restored IR in the interpreter")
        must(sent >= 0 and fb > sent, "AC2: the sentinel gates the interpreter fallback")

    # ── AC3: runtime test face ─────────────────────────────────────
    must("ac4606_prim_word_refuse" in tst, "AC3: ac4606_prim_word_refuse defined")
    must("ac4606_prim_word_refuse();" in tst, "AC3: refuse face registered in the runner")
    must("ac4606_source_cite();" in tst, "AC3: source-cite face registered in the runner")
    must("ac4606_gate_wiring();" in tst, "AC3: gate-wiring face registered in the runner")
    must("make_ref(vt::RefPrimitive, 58) == 3733" in tst, "AC3: string? raw word pinned as make_ref(RefPrimitive, 58)")
    must(
        "make_ref(vt::RefPrimitive, 371) == 23765" in tst, "AC3: getenv raw word pinned as make_ref(RefPrimitive, 371)"
    )
    must("aura_jit_peek_prologue_deopt_sentinel" in tst, "AC3: sentinel armed observable")
    must("aura_jit_take_prologue_deopt_sentinel" in tst, "AC3: sentinel take/consume observable")
    must("make_ref(vt::RefCell, 3)" in tst, "AC3: non-prim ref word keeps the callee_val echo")
    must(not list(ROOT.glob("tests/**/test_issue_4606*.cpp")), "AC3: no dedicated test_issue_4606 file (#81934)")
    must(not list(ROOT.glob("docs/design/4606-*")), "AC3: no docs/design/4606 markdown (#1655)")

    # ── AC4: wiring ────────────────────────────────────────────────
    must("check_jit_prim_callee_refuse_4606.py" in build, "AC4: build.py registers this linter")
    must("check_jit_prim_callee_refuse_4606.py" in allow, "AC4: root_check_allowlist.txt lists this linter")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
