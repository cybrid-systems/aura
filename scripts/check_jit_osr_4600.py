#!/usr/bin/env python3
"""Issue #4600 source-cite gate (phase 3: §3): named-let OSR — the
interpreted named-let loop escapes to the module's registered native
code instead of running every remaining iteration in the IR interpreter.

Phase 1 (§4/§5/§7) is pinned by scripts/check_jit_assumption_gate_4600.py;
phase 2 (§1/§2) by scripts/check_jit_frame_deopt_4600.py. This phase:

  §3  OSR at named-let back-edges. Agent-written code is often a single
      long loop; after a guard deopt (§1/§2) the resumed frame runs the
      loop interpreted — the drain's tail-call replacement (#4459) is
      the named-let back-edge as the interpreter sees it. drain_call_stack
      counts consecutive same-function replacements; past
      AURA_JIT_OSR_N it marshals the pended frame's env+args (exactly
      the native entry's OpArg inputs) and hands the chain to the OSR
      bridge, which calls the fn's REGISTERED ScalarFn — no compile, the
      code exec_jit already built and registered. The native value
      completes the replaced frame (top frames return it, callers see it
      in their result slot — the drain's own completion protocol). The
      host carries a busy guard so an OSR'd native entry that deopts
      again (forced-deopt mode) resumes interpreted instead of nesting
      OSR→deopt cycles. AURA_JIT_OSR=0 declines at the bridge; the
      escape is only armed inside the exec_jit deopt-host window.

ACs:
  AC1  aura_jit_bridge.h/.cpp + aura_jit_bridge_stub.cpp: the C ABI
       (aura_jit_osr_enter, aura_jit_set_osr_host, aura_jit_osr_fn),
       the AURA_JIT_OSR=0 door, the osr_tail_fires bump on handled
       calls, the decline contract (no host / null args / host 0), and
       weak light-link stubs that keep interpreted loops running.
  AC2  ir_executor_impl.cpp: the drain-loop tail-replacement hook —
       same-func chain counter (reset on a different callee), the
       AURA_JIT_OSR_N threshold (default 64), env+args marshalled as
       EvalValue bits, and the completion protocol (top frames return
       the value, callers store into their result slot).
  AC3  service.ixx: JitDeoptHostState carries the registered-native
       table (jit); jit_osr_enter_host_impl looks the fn up and calls
       it with the marshalled buffer; the busy guard breaks OSR→deopt
       nesting; JitDeoptHostArm arms/disarms the OSR host with the
       frame-deopt host around the exec_jit entry call.
  AC4  jit_assumptions.hh: osr_tail_fires in Stats and rendered by
       format_stats (query:jit-assumption-stats visibility).
  AC5  Runtime doors: tests/compiler/test_mutate_type_gate.cpp
       jit_osr_tail_4600 (strong-ABI resolution, unhosted decline, host
       roundtrip, host-decline propagation, degenerate args) registered
       in the runner; tests/python/test_regression.py test_jit_osr_
       tail_4600 (byte-identical across on/off/threshold/forced-deopt,
       ir-only control, session fires>=1 / quiet==0). No
       tests/**/test_issue_4600*.cpp, no docs/design/4600-* (per #1655).
  AC6  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BR = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRH = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRS = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
IEI = ROOT / "src" / "compiler" / "ir_executor_impl.cpp"
JA = ROOT / "src" / "compiler" / "jit_assumptions.hh"
SV = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_mutate_type_gate.cpp"
PYT = ROOT / "tests" / "python" / "test_regression.py"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def main() -> int:
    br = BR.read_text()
    brh = BRH.read_text()
    brs = BRS.read_text()
    iei = IEI.read_text()
    ja = JA.read_text()
    sv = SV.read_text()
    tst = TST.read_text()
    pyt = PYT.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: bridge ABI ─────────────────────────────────────────────
    must(
        "aura_jit_osr_fn" in brh and "aura_jit_osr_enter" in brh and "aura_jit_set_osr_host" in brh,
        "AC1: header declares the OSR C ABI",
    )
    must('std::getenv("AURA_JIT_OSR")' in br, "AC1: AURA_JIT_OSR=0 door at the bridge")
    drain_block = br[br.find("aura_jit_osr_enter(const char* fn_name") :]
    must("osr_tail_fires.fetch_add" in drain_block, "AC1: handled host call bumps osr_tail_fires")
    must(
        "g_osr_host" in br and "return 0;" in drain_block, "AC1: bridge declines without a host (interpreted fallback)"
    )
    must(
        "aura_jit_osr_enter" in brs and "aura_jit_set_osr_host" in brs,
        "AC1: weak light-link stubs keep interpreted loops running",
    )

    # ── AC2: drain hook ─────────────────────────────────────────────
    must('std::getenv("AURA_JIT_OSR_N")' in iei, "AC2: AURA_JIT_OSR_N threshold door")
    drain_fn = iei[iei.find("EvalResult IRInterpreter::drain_call_stack") :]
    drain_fn = drain_fn[: drain_fn.find("\nEvalResult")]
    must(
        "osr_tail_self" in drain_fn and "pc.func == call_stack_.back().func" in drain_fn,
        "AC2: same-func tail chain counter in the drain tail branch",
    )
    must("osr_tail_self = 0;" in drain_fn, "AC2: different callee resets the chain count")
    must(
        "aura_jit_osr_enter(" in drain_fn and "pc.args[i].val" in drain_fn,
        "AC2: bridge called with the pended frame's env+args bits",
    )
    must(
        "call_stack_.back().locals[osr_slot] = osr_val;" in drain_fn,
        "AC2: caller frames receive the value in their result slot",
    )

    # ── AC3: host arm ───────────────────────────────────────────────
    host_state = sv[sv.find("struct JitDeoptHostState") :]
    host_state = host_state[: host_state.find("};")]
    must("aura::jit::AuraJIT* jit" in host_state, "AC3: host state carries the registered-native table")
    must(
        "jit_osr_enter_host_impl" in sv and "get_function_ptr(fn_name)" in sv,
        "AC3: OSR host resolves the fn's registered ScalarFn",
    )
    must("t_jit_osr_busy" in sv, "AC3: busy guard breaks OSR→deopt nesting")
    arm_block = sv[sv.find("struct JitDeoptHostArm") :]
    arm_block = arm_block[: arm_block.find("};")]
    must(
        "aura_jit_set_osr_host(&jit_osr_enter_host_impl)" in arm_block
        and "aura_jit_set_osr_host(nullptr)" in arm_block,
        "AC3: JitDeoptHostArm arms/disarms the OSR host",
    )
    must("&evaluator_, &jit_}" in sv, "AC3: exec_jit arms the host state with the fn table")

    # ── AC4: stats ──────────────────────────────────────────────────
    must(
        "osr_tail_fires" in ja and 'kv("osr_tail_fires"' in ja, "AC4: stats struct + format_stats render osr_tail_fires"
    )

    # ── AC5: runtime doors ──────────────────────────────────────────
    must(
        "jit_osr_tail_4600" in tst and "jit_osr_tail_4600();" in tst,
        "AC5: C++ door jit_osr_tail_4600 registered in the runner",
    )
    must(
        "test_jit_osr_tail_4600" in pyt and "test_jit_osr_tail_4600," in pyt,
        "AC5: python net jit_osr_tail_4600 registered in the runner list",
    )
    must(
        '"AURA_JIT_OSR"' in pyt and '"AURA_JIT_OSR_N"' in pyt and "AURA_JIT_FORCE_GUARD_DEOPT" in pyt,
        "AC5: python net covers on/off/threshold/forced-deopt faces",
    )
    must(
        "test_issue_4600" not in str(list(ROOT.glob("tests/**/*4600*"))),
        "AC5: no dedicated test_issue_4600 file (#81934)",
    )
    must(not list(ROOT.glob("docs/design/4600-*")), "AC5: no docs/design/4600 markdown (#1655)")

    # ── AC6: wiring ────────────────────────────────────────────────
    must("check_jit_osr_4600.py" in build, "AC6: build.py registers this linter")
    must("check_jit_osr_4600.py" in allow, "AC6: root_check_allowlist.txt lists this linter")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
