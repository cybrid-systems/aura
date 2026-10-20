#!/usr/bin/env python3
"""Issue #4600 source-cite gate (phase 1: §4/§5/§7): prologue
assumption-word gate, direct defuse-word loads on car/cdr guards, and
the deopt-storm policy.

Follow-up to #4569 (assumption-based invalidation). Shipped in this
phase:

  §7  The Apply prologue no longer calls aura_jit_get_current_bridge_epoch
      + aura_jit_is_fn_epoch_stale on every native call. One acquire load
      of the fn's DefineVersion word answers "not stale" on the hot path
      (same predicate the #4569 cache-hit restamp uses); the epoch calls
      survive only on the cold moved path. AURA_JIT_PROLOGUE_WORD=0 and
      global-epoch mode keep the old unconditional prologue.
  §5  OpCar/OpCdr SHAPE_PAIR guards stop reading the global defuse
      version through one get_version hook call per access. The bridge
      publishes the Evaluator's defuse atomic (stable address,
      aura_jit_set_defuse_word) and each guard lowers to two acquire
      loads (slot -> word). AURA_JIT_DEFUSE_WORD=0 restores the call
      lowering. A hookless process keeps the 0==0 fast path via the
      fallback word.
  §4  Deopt-storm policy: prologue deopts and car/cdr guard slow paths
      count per fn (jit_assume::DeoptStorms, threshold
      AURA_JIT_DEOPT_STORM_N, default 3). A stormed fn is marked
      megamorphic in the ShapeProfiler on the next exec_jit compile
      probe — stability cleared, profile version bumped (shape-
      specialized cache entries miss), re-stabilization blocked — so
      the recompile drops that speculation.

ACs:
  AC1  aura_jit.cpp §7: jit_prologue_word_enabled kill switch; prologue
       emits prologue_word_ok / prologue_epoch_chk / prologue_stale_join
       with the word load acquire + branch-likely; the gate arms
       prologue_word_armed and is skipped under global_epoch_mode().
  AC2  aura_jit.cpp §5: both OpCar and OpCdr spec_pair arms load the
       defuse word through aura_jit_defuse_word_slot_addr() with
       Acquire ordering behind jit_defuse_word_enabled(); both bb_slow
       arms attribute via fn_note_guard_deopt (named fns only).
  AC3  aura_jit_runtime.cpp §5/§4: fallback word + aura_jit_set_defuse_word
       + aura_jit_defuse_word_slot_addr + aura_jit_note_guard_deopt.
  AC4  jit_assumptions.hh §4: DeoptStorms (threshold env
       AURA_JIT_DEOPT_STORM_N, stormed/count/clear_for_test) and the
       prologue_word_armed / guard_deopt_notes / storm_marks stats
       fields rendered by format_stats.
  AC5  evaluator.ixx: defuse_version_word() accessor.
  AC6  service.ixx: install_lock_and_topcell_hooks_for_eval publishes
       the word; the exec_jit compile probe storms the profiler
       (DeoptStorms::stormed -> mark_megamorphic).
  AC7  shape_profiler.h/.cpp: FnProfile.mega + mark_megamorphic
       (stability cleared, version bumped) + sticky gates on is_stable
       and the record path.
  AC8  aura_jit_bridge.cpp: aura_jit_deopt_to_interpreter notes
       DeoptStorms for named fns.
  AC9  main.cpp: --jit-session ;;storm NAME [N] directive.
  AC10 Runtime doors: tests/compiler/test_mutate_type_gate.cpp
       jit_word_and_storm_4600 registered in run_test_mutate_type_gate;
       tests/python/test_regression.py test_jit_prologue_word_and_storm_4600
       registered in the tf list. No tests/**/test_issue_4600.cpp, no
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
RT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
JA = ROOT / "src" / "compiler" / "jit_assumptions.hh"
EV = ROOT / "src" / "compiler" / "evaluator.ixx"
SV = ROOT / "src" / "compiler" / "service.ixx"
BR = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
SPH = ROOT / "src" / "compiler" / "shape_profiler.h"
SPC = ROOT / "src" / "compiler" / "shape_profiler.cpp"
MN = ROOT / "src" / "main.cpp"
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


def _between_call(text: str, case: str, call: str, end: str) -> str:
    # Window from the first call site inside the case up to end.
    #
    # The call argument list is clang-format reflowed, so the argument
    # text is not a stable anchor; the case label and the call name are.
    i = text.find(case)
    if i < 0:
        return ""
    j = text.find(call, i)
    if j < 0:
        return ""
    k = text.find(end, j)
    return text[j : k if k > 0 else len(text)]


def main() -> int:
    aj = AJ.read_text()
    rt = RT.read_text()
    ja = JA.read_text()
    ev = EV.read_text()
    sv = SV.read_text()
    br = BR.read_text()
    sph = SPH.read_text()
    spc = SPC.read_text()
    mn = MN.read_text()
    tst = TST.read_text()
    pyt = PYT.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: §7 prologue assumption-word gate ───────────────────────
    must("#4600" in aj, "AC1: aura_jit.cpp cites #4600")
    must("AURA_JIT_PROLOGUE_WORD" in aj, "AC1: prologue gate kill switch env")
    prologue = _between(aj, "const bool word_gate =", "// UINT32_MAX")
    must(bool(prologue), "AC1: prologue word-gate window found")
    must(
        "DefineVersions::instance().word(fn.name)" in prologue,
        "AC1: gate loads the fn's DefineVersion word",
    )
    must("prologue_word_armed" in prologue, "AC1: armed counter bump")
    must(
        "AtomicOrdering::Acquire" in prologue and "CreateICmpNE" in prologue,
        "AC1: acquire word load + moved compare",
    )
    must(
        '"prologue_word_ok"' in prologue and '"prologue_epoch_chk"' in prologue and '"prologue_stale_join"' in prologue,
        "AC1: hot/cold/join block structure",
    )
    must("br_likely" in prologue, "AC1: hot edge is the word-ok branch")
    must(
        "global_epoch_mode()" in prologue,
        "AC1: gate skipped in global-epoch mode",
    )
    must(
        aj.count("aura_jit_is_fn_epoch_stale") >= 2,
        "AC1: epoch C calls kept (cold path + decl)",
    )

    # ── AC2: §5 direct defuse word on car/cdr guards ────────────────
    must("AURA_JIT_DEFUSE_WORD" in aj, "AC2: defuse word kill switch env")
    # Issue #4603 reflowed the emit_l2_pair_field call across lines, so its
    # argument text is no longer a contiguous literal. Anchor on the case
    # label and the call name instead - both are stable.
    car = _between_call(aj, "case OpCar:", "emit_l2_pair_field(", "case OpCdr:")
    cdr = _between_call(aj, "case OpCdr:", "emit_l2_pair_field(", "// CastOp: runtime type check")
    must(bool(car) and bool(cdr), "AC2: car/cdr lowering windows found")
    for name, win in (("car", car), ("cdr", cdr)):
        must(
            "aura_jit_defuse_word_slot_addr()" in win,
            f"AC2: Op{name} guard reads the word slot directly",
        )
        must(
            win.count("AtomicOrdering::Acquire") >= 2,
            f"AC2: Op{name} slot+word loads are acquire",
        )
        must(
            "jit_defuse_word_enabled()" in win,
            f"AC2: Op{name} keeps the call lowering behind the kill switch",
        )
        must(
            "fn_note_guard_deopt" in win,
            f"AC2: Op{name} bb_slow attributes the guard deopt (#4600 §4)",
        )

    # ── AC3: runtime word slot + storm note ─────────────────────────
    must("g_jit_defuse_word_fallback" in rt, "AC3: hookless fallback word")
    must("aura_jit_set_defuse_word" in rt, "AC3: word publish entry point")
    must("aura_jit_defuse_word_slot_addr" in rt, "AC3: slot address helper")
    must(
        "DeoptStorms::instance().note(name)" in rt and "guard_deopt_notes" in rt,
        "AC3: guard-deopt note feeds the storm policy",
    )

    # ── AC4: DeoptStorms + stats fields ─────────────────────────────
    must("class DeoptStorms" in ja, "AC4: DeoptStorms registry")
    must("AURA_JIT_DEOPT_STORM_N" in ja, "AC4: storm threshold env")
    must("bool stormed(" in ja and "void clear_for_test()" in ja, "AC4: storm predicate + reset")
    for f in ("prologue_word_armed", "guard_deopt_notes", "storm_marks"):
        must(f'kv("{f}"' in ja, f"AC4: format_stats renders {f}")

    # ── AC5: evaluator word accessor ────────────────────────────────
    must(
        "defuse_version_word()" in ev,
        "AC5: Evaluator exposes the defuse atomic's address",
    )

    # ── AC6: service wiring ─────────────────────────────────────────
    must(
        "aura_jit_set_defuse_word(ev->defuse_version_word())" in sv,
        "AC6: hook install publishes the word",
    )
    must(
        "DeoptStorms::instance().stormed(" in sv and "mark_megamorphic(fn_key)" in sv,
        "AC6: compile probe storms the profiler",
    )

    # ── AC7: profiler megamorphic mark ──────────────────────────────
    must("bool mega = false;" in sph, "AC7: FnProfile.mega flag")
    must("void mark_megamorphic(FnKey fn);" in sph, "AC7: mark_megamorphic declared")
    mark = _between(spc, "void ShapeProfiler::mark_megamorphic(FnKey fn) {", "\n}\n")
    must(bool(mark), "AC7: mark_megamorphic implemented")
    must(
        "profile.is_stable = false;" in mark and "shape_version_bump_count" in mark,
        "AC7: mark clears stability + bumps version",
    )
    must(
        spc.count("!it->second.mega") >= 2,
        "AC7: is_stable paths gate on mega",
    )
    must(
        "if (profile.mega)" in spc,
        "AC7: record path never re-stabilizes a storm-marked profile",
    )

    # ── AC8: prologue-deopt storm attribution ───────────────────────
    bridge = _between(
        br,
        'extern "C" std::int64_t aura_jit_deopt_to_interpreter(const char* name) {',
        "return 0; // fixnum 0 sentinel",
    )
    must(
        "DeoptStorms::instance().note(name)" in bridge,
        "AC8: prologue deopt notes the storm policy",
    )

    # ── AC9: ;;storm directive ──────────────────────────────────────
    must(
        'line.starts_with(";;storm ")' in mn,
        "AC9: --jit-session ;;storm directive",
    )

    # ── AC10: runtime doors ─────────────────────────────────────────
    must(
        "jit_word_and_storm_4600()" in tst and tst.count("jit_word_and_storm_4600()") >= 2,
        "AC10: cpp unit door defined + registered",
    )
    must(
        "def test_jit_prologue_word_and_storm_4600():" in pyt and "test_jit_prologue_word_and_storm_4600," in pyt,
        "AC10: python regression door defined + registered",
    )
    must(
        not list((ROOT / "tests").rglob("test_issue_4600*.cpp")) and not list((ROOT / "docs" / "design").glob("4600-*"))
        if (ROOT / "docs" / "design").exists()
        else True,
        "AC10: no test_issue_4600.cpp / docs/design 4600 (per #1655/#81934)",
    )

    # ── AC11: wiring ────────────────────────────────────────────────
    must(
        "check_jit_assumption_gate_4600.py" in build,
        "AC11: build.py registers this linter",
    )
    must(
        "check_jit_assumption_gate_4600.py" in allow,
        "AC11: coverage-policy allowlist lists this linter",
    )

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
