#!/usr/bin/env python3
"""Issue #4600 source-cite gate (phase 2: §1/§2): guard exits share one
frame-state deopt stub; a failed guard resumes the IR interpreter at the
guard's (block, instr) with the spilled live locals instead of carrying
an inline generic fallback (or erroring out prologue-only).

Phase 1 (§4/§5/§7) is pinned by scripts/check_jit_assumption_gate_4600.py.
This phase:

  §1  Guards that exit. OpCar/OpCdr spec_pair guards and the
      OpGuardShape epoch arm, plus the Apply prologue stale path and the
      entry shape-guard miss, branch to one shared stub call
      (aura_jit_deopt_frame) in the cold block. The old inline generic
      fallbacks survive only for un-armed fns (AURA_JIT_FRAME_DEOPT=0,
      AOT, anon fns, hidden pcell/spair state).
  §2  Frame-state deopt to the IR interpreter. Each guard exit spills
      the live IR locals (tagged i64 = EvalValue bits; float shadows
      reboxed through the lowering's load()) plus the resume
      (block, instr). The stub dispatches to the exec_jit-armed host,
      which materializes an IRInterpreter frame at that resume point
      (resume_deopt_frame — the #2292 post-PendingCall re-entry) and
      returns the completed call's value; without a host the stub
      degrades to the #4084 fixnum-0 sentinel protocol so callers fall
      back one level up. Guard deopts attribute to the §4 deopt-storm
      policy.

ACs:
  AC1  aura_jit.cpp §1/§2: jit_frame_deopt_enabled /
       jit_force_guard_deopt env doors (AURA_JIT_FRAME_DEOPT,
       AURA_JIT_FORCE_GUARD_DEOPT 1|2) and the fn_deopt_frame stub
       declaration + ORC reg() row.
  AC2  aura_jit.cpp §1/§2: builder frame_deopt_ok gating (named, not
       AOT, local_count>0, no pcell/spair hidden state), entry_block_idx
       + cur_block/cur_instr resume points, the spill_buf entry alloca,
       and emit_frame_deopt_exit (explicit per-guard spill through
       load() — float shadows reboxed — one stub call, ret).
  AC3  aura_jit.cpp §1: OpCar + OpCdr armed forms — pair_guard_exit
       compare + car_frame_deopt / cdr_frame_deopt exits; force mode
       speculates the pair shape with a forced-false compare; the
       un-armed inline 3-block fallback stays for everyone else.
  AC4  aura_jit.cpp §2: the Apply prologue stale path and the entry
       shape-guard miss call emit_frame_deopt_exit(from_entry=true);
       force levels 2 (prologue) and >=1 (shape guard) drive them.
  AC5  aura_jit.cpp §1: the OpGuardShape stale arm exits to the stub
       (frame_deopt_ok) — resume re-executes the guard in the
       interpreter — with the store-false inline fallback un-armed.
  AC6  aura_jit_bridge.h/.cpp + aura_jit_bridge_stub.cpp §1/§2: the
       C ABI (aura_jit_deopt_frame, aura_jit_set_deopt_frame_host,
       env/arg-count lookups), DeoptStorms attribution, frame_deopt_*
       stats, the unhosted sentinel degrade, and weak light-link stubs.
  AC7  ir_executor.ixx / ir_executor_impl.cpp §2: resume_deopt_frame —
       ExecFrame at (block, instr) with args, is_top_level, drained
       via drain_call_stack; range checks on block/instr; void pad.
  AC8  service.ixx §2: JitDeoptHostArm arms the host around the
       exec_jit entry native call; the host builds an IRContext +
       IRInterpreter over the sampled module, slices env++args for
       entry resumes, and degrades to the sentinel on any miss.
  AC9  jit_assumptions.hh: frame_deopt_armed / frame_deopt_exits /
       frame_deopt_resumes / frame_deopt_unhosted stats rendered by
       format_stats.
  AC10 Runtime doors: tests/compiler/test_mutate_type_gate.cpp
       jit_frame_deopt_4600 (stub ABI, unhosted degrade, storm
       attribution, host roundtrip) registered in
       run_test_mutate_type_gate; tests/python/test_regression.py
       test_jit_soundness_4567 runs jitf1/jitf2 forced-deopt-at-
       every-guard modes byte-identical against ir/eval/jit (the §2
       acceptance). No tests/**/test_issue_4600*.cpp, no
       docs/design/4600-* (per #1655).
  AC11 Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AJ = ROOT / "src" / "compiler" / "aura_jit.cpp"
BR = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRH = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRS = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
IEX = ROOT / "src" / "compiler" / "ir_executor.ixx"
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


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def main() -> int:
    aj = AJ.read_text()
    br = BR.read_text()
    brh = BRH.read_text()
    brs = BRS.read_text()
    iex = IEX.read_text()
    iei = IEI.read_text()
    ja = JA.read_text()
    sv = SV.read_text()
    tst = TST.read_text()
    pyt = PYT.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: env doors + stub declaration and registration ──────────
    must("AURA_JIT_FRAME_DEOPT" in aj, "AC1: frame-deopt kill switch env")
    must("AURA_JIT_FORCE_GUARD_DEOPT" in aj, "AC1: forced-deopt mode env")
    must('reg("aura_jit_deopt_frame"' in aj, "AC1: ORC reg row for the stub")
    must(
        "aura_jit_deopt_frame" in brh and "aura_jit_deopt_frame_fn" in brh,
        "AC1: bridge header declares the stub ABI + host fn type",
    )

    # ── AC2: builder gating, resume points, spill, exit emitter ─────
    arming = _between(aj, "builder.frame_deopt_ok = false;", "bool reads_pairs")
    must(bool(arming), "AC2: frame_deopt_ok arming window found")
    must(
        "builder.is_pcell(i)" in arming and "builder.is_spair" in arming,
        "AC2: hidden pcell/spair state disarms guard exits",
    )
    must(
        "entry_block_idx" in arming and "spill_buf" in arming and "frame_deopt_armed" in arming,
        "AC2: entry block index, spill alloca, armed counter",
    )
    must(
        "builder.cur_block = bi;" in aj and "builder.cur_instr = ii;" in aj,
        "AC2: emit loop records the (block, instr) resume point",
    )
    emit = _between(aj, "void emit_frame_deopt_exit(", "llvm::Value* c64(int64_t v)")
    must(bool(emit), "AC2: emit_frame_deopt_exit found")
    must("irb->CreateStore(load(i), gep);" in emit, "AC2: explicit spill through load() (float shadows reboxed)")
    must("from_entry" in emit and "func->arg_begin()" in emit, "AC2: entry resumes source the caller's locals buffer")

    # ── AC3: OpCar/OpCdr armed guard-exit forms ─────────────────────
    for op in ("car", "cdr"):
        _between(aj, "const bool pair_guard_exit = frame_deopt_ok;", f'"{op}_frame_deopt"')
        full = _between(
            aj,
            "const bool pair_guard_exit = frame_deopt_ok;",
            "return true;\n                }\n                // Issue #4600 §1: un-armed form",
        )
        must(f'"{op}_frame_deopt"' in aj, f"AC3: Op{op.capitalize()} frame-deopt exit block")
        must(
            "pair_force" in full and "getFalse" in full,
            f"AC3: Op{op.capitalize()} force mode speculates with forced mismatch",
        )
        arm_win = _between(aj, f'"{op}_frame_deopt"', "// Issue #4600 §1: un-armed form")
        must(f"fn_pair_{op}_unchecked" in arm_win, f"AC3: Op{op.capitalize()} armed fast path keeps the unchecked read")
    must(aj.count("const bool pair_guard_exit = frame_deopt_ok;") == 2, "AC3: both OpCar and OpCdr converted")

    # ── AC4: prologue + entry shape guard ───────────────────────────
    prologue = _between(
        aj, "Issue #4600 §2: force level 2 makes every prologue stale", "builder.entry_body_bb = bb_cont;"
    )
    must(
        "emit_frame_deopt_exit(fn, builder.entry_block_idx, 0," in aj,
        "AC4: prologue stale path exits to the frame-deopt stub",
    )
    must("jit_force_guard_deopt() >= 2" in prologue, "AC4: force level 2 drives the prologue stale")
    shape_guard = _between(aj, "auto* miss = builder.irb->CreateICmpEQ(guard_ok", "builder.entry_body_bb = bb_cont;")
    must(
        "emit_frame_deopt_exit" in shape_guard and "jit_force_guard_deopt() >= 1" in shape_guard,
        "AC4: entry shape-guard miss exits to the stub (force >= 1)",
    )

    # ── AC5: OpGuardShape stale arm ─────────────────────────────────
    gs = _between(aj, "case OpGuardShape:", "case OpJump:")
    must("emit_frame_deopt_exit(fn, cur_block, cur_instr" in gs, "AC5: GuardShape stale arm exits to the shared stub")
    must("KWD_FALSE_VAL" in gs, "AC5: un-armed GuardShape keeps the store-false inline fallback")

    # ── AC6: bridge stub + weak light-link fallbacks ────────────────
    body = _between(br, 'extern "C" std::int64_t aura_jit_deopt_frame(', "// ── Issue #4083")
    must(bool(body), "AC6: bridge stub body found")
    must(
        "frame_deopt_exits" in body and "DeoptStorms::instance().note" in body,
        "AC6: stub bumps exit counter + attributes DeoptStorms",
    )
    must(
        "frame_deopt_resumes" in body and "frame_deopt_unhosted" in body,
        "AC6: stub counts resumes vs unhosted degrades",
    )
    must("aura_jit_note_prologue_deopt_sentinel()" in body, "AC6: unhosted degrade arms the #4084 sentinel")
    must("aura_lookup_fn_by_name" in br, "AC6: env/arg counts resolve through the runtime name table")
    must(
        "__attribute__((weak))" in brs and "aura_jit_deopt_frame" in brs, "AC6: light links resolve weak stub fallbacks"
    )

    # ── AC7: interpreter resume entry ───────────────────────────────
    must("resume_deopt_frame" in iex, "AC7: IRInterpreter declares resume_deopt_frame")
    impl = _between(
        iei, "EvalResult IRInterpreter::resume_deopt_frame(", "IRInterpreter::RunResult IRInterpreter::run_function"
    )
    must(bool(impl), "AC7: resume_deopt_frame implementation found")
    must(
        ".current_block = block" in impl and ".resume_instr = instr" in impl and ".is_top_level = true" in impl,
        "AC7: frame carries (block, instr) resume + top-level drain",
    )
    must("resume block out of range" in impl and "resume instr out of range" in impl, "AC7: block/instr range checks")
    must("make_void()" in impl, "AC7: void pad beyond the spilled slots")

    # ── AC8: exec_jit host arm ──────────────────────────────────────
    host = _between(sv, "struct JitDeoptHostState", 'extern "C" std::int64_t aura_jit_prim_dispatch')
    must("JitDeoptHostArm deopt_host_4600" in sv, "AC8: exec_jit arms the deopt host around the entry call")
    must(
        "resume_deopt_frame" in host and "IRContext ctx" in host,
        "AC8: host resumes through a fresh IRInterpreter frame",
    )
    must(
        "aura_jit_deopt_frame_env_count" in host and "entry_block" in host,
        "AC8: entry resumes slice env++args from the caller buffer",
    )
    must("aura_jit_note_prologue_deopt_sentinel()" in host, "AC8: host degrades to the sentinel on any miss")

    # ── AC9: stats ──────────────────────────────────────────────────
    must(
        all(k in ja for k in ("frame_deopt_armed", "frame_deopt_exits", "frame_deopt_resumes", "frame_deopt_unhosted")),
        "AC9: stats struct carries the frame-deopt counters",
    )
    must(
        'kv("frame_deopt_resumes"' in ja and 'kv("frame_deopt_unhosted"' in ja,
        "AC9: format_stats renders the frame-deopt rows",
    )

    # ── AC10: runtime doors ─────────────────────────────────────────
    must(
        "jit_frame_deopt_4600" in tst and "jit_frame_deopt_4600();" in tst,
        "AC10: C++ door jit_frame_deopt_4600 registered in the runner",
    )
    must(
        '"jitf1"' in pyt and '"jitf2"' in pyt and "AURA_JIT_FORCE_GUARD_DEOPT" in pyt,
        "AC10: python corpus runs the forced-deopt modes (§2 acceptance)",
    )
    must(
        "test_issue_4600" not in str(list(ROOT.glob("tests/**/*4600*"))),
        "AC10: no dedicated test_issue_4600 file (#81934)",
    )
    must(not list(ROOT.glob("docs/design/4600-*")), "AC10: no docs/design/4600 markdown (#1655)")

    # ── AC11: wiring ────────────────────────────────────────────────
    must("check_jit_frame_deopt_4600.py" in build, "AC11: build.py registers this linter")
    must("check_jit_frame_deopt_4600.py" in allow, "AC11: root_check_allowlist.txt lists this linter")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
