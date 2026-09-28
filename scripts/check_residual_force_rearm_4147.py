#!/usr/bin/env python3
"""Issue #4147 source-cite gate: residual-force auto-heal re-arm.

`HotUpdateRegistry::observe_residual_force_stale` fires one bounded
auto-heal per residual/force mask generation
(`residual_force_auto_heal_last_mask_`, kAutoHealExits=256). The reload
recovery playbook is observe-only (#2953 / #3026) — it never executes
FallBackJit itself. When that one heal's decide_and_reemit /
coverage-verify returned n==0 (Defer, storm, empty candidates) or
only_covered left residual_force_mask() unchanged, the cap blocked every
later retry while the Agent never acted on the playbook hint: a sticky
force_jit_regions_mask / residual under long-running high-freq self-mod
(fail-closed demotion, but zero-downtime re-promote dead until manual
intervention).

#4147 re-arms the belt: after the ResidualForceHeal pass the residual
path re-reads residual_force_mask() and clears
residual_force_auto_heal_last_mask_ when it still equals the armed
generation, so a later 256-exit BoundaryExit window can retry — still at
most one in-flight heal per window (the age reset gates), still Soft/Off
zero-cost (early-returned before the auto-heal check), still no playbook
auto-execution. A shrunk / cleared residual keeps the cap armed; the
prev != gen branch re-arms the new generation on the next observe. The
FallBackJit path cannot no-op (its covered clear always changes the
face), so no re-arm is needed there.

ACs:
  AC1  residual path re-arms on a no-op heal: after
       maybe_coverage_verify_min_dirty(ReemitReason::ResidualForceHeal)
       the code compares residual_force_mask() == gen and clears
       residual_force_auto_heal_last_mask_ (store 0); the arm
       (store gen) precedes the heal call, the re-arm follows it, and
       the observe impl cites Issue #4147.
  AC2  window + pre-heal gates preserved: kAutoHealExits=256 gate,
       observe age reset before the heal, exhausted-retry-budget check
       (attempts_left load) and storm check still gate the fire.
  AC3  FallBackJit face (#3814) unchanged: the covered clear block still
       stores force_jit_regions_mask_ 0, clears eval force slots, and
       bumps force_jit_repromote_total_ — no re-arm inside it (the face
       always changes), and the doc comment states the #4147 contract.
  AC4  Soft / Off unchanged: the production_defaults probe early-return
       stays the first check of observe_residual_force_stale.
  AC5  runtime doors: tests/compiler/test_issue_3096.cpp cites #4147
       with the four ac4147_* runners each dispatched exactly once and
       the restated AC3 fixture; test_reload_recovery_query.cpp's
       ac3248_1 re-arm restatement is present; no
       tests/compiler/test_issue_4147.cpp; no docs/design/4147-*; no new
       query key.
  AC6  wiring: build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
TST3096 = ROOT / "tests" / "compiler" / "test_issue_3096.cpp"
TSTREC = ROOT / "tests" / "compiler" / "test_reload_recovery_query.cpp"

FN = "void HotUpdateRegistry::observe_residual_force_stale() noexcept {"


def _strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        out.append(line[:cut] if cut >= 0 else line)
    return "\n".join(out)


def main() -> int:
    cpp_raw = CPP.read_text() if CPP.exists() else ""
    cpp = _strip_comments(cpp_raw)
    tst3096 = TST3096.read_text() if TST3096.exists() else ""
    tstrec = TSTREC.read_text() if TSTREC.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        if not good:
            ok = False
        print(f"  [{'PASS' if good else 'FAIL'}] {name}: {msg}")

    fn = cpp.find(FN)
    report("AC0.fn", fn != -1, "observe_residual_force_stale present in hot_update_registry.cpp")
    win = cpp[fn : fn + 4600] if fn != -1 else ""
    # Anchor the raw window at the raw-text position — comment stripping
    # shifts offsets, so the stripped index is wrong for cpp_raw slices.
    fn_raw = cpp_raw.find(FN)
    win_raw = cpp_raw[fn_raw : fn_raw + 5600] if fn_raw != -1 else ""

    # AC1 — residual path re-arms on a no-op heal (ordered arm → heal →
    # re-read → clear-cap).
    heal = win.find("maybe_coverage_verify_min_dirty(ReemitReason::ResidualForceHeal)")
    arm = win.find("residual_force_auto_heal_last_mask_.store(gen")
    cmp_unchanged = win.find("residual_force_mask() == gen")
    clear_cap = win.find("residual_force_auto_heal_last_mask_.store(0", heal if heal != -1 else 0)
    report(
        "AC1.rearm",
        heal != -1 and cmp_unchanged != -1 and clear_cap != -1 and clear_cap > heal,
        "residual path compares residual_force_mask() == gen after the heal and clears the cap",
    )
    report(
        "AC1.order",
        0 <= arm < heal and heal < cmp_unchanged,
        "arm (store gen) precedes the heal; the re-arm comparison follows it",
    )
    report(
        "AC1.cite",
        "Issue #4147" in win_raw,
        "observe impl cites Issue #4147",
    )

    # AC2 — window + pre-heal gates preserved.
    age_reset = win.find("residual_force_observe_age_.store(0")
    gate = win.find("age >= kAutoHealExits")
    attempts = win.find("exhausted_min_dirty_retry_attempts_left_.load")
    storm = win.find("current_storm_level() != StormLevel::None")
    report(
        "AC2.window",
        gate != -1 and age_reset != -1 and age_reset < heal,
        "kAutoHealExits gate kept; observe age reset precedes the heal (one in-flight heal per window)",
    )
    report(
        "AC2.gates",
        attempts != -1 and storm != -1 and attempts < heal and storm < heal,
        "exhausted-retry-budget and storm checks still gate the fire",
    )

    # AC3 — FallBackJit face (#3814) unchanged; no re-arm inside the clear.
    force_clear = win.find("force_jit_regions_mask_.store(0", heal if heal != -1 else 0)
    slots = win.find("clear_eval_force_slots()", heal if heal != -1 else 0)
    repromote = win.find("force_jit_repromote_total_.fetch_add", heal if heal != -1 else 0)
    no_rearm_in_clear = force_clear == -1 or win.find("residual_force_mask() == gen", force_clear) == -1
    report(
        "AC3.fallback_jit_clear",
        force_clear != -1 and slots != -1 and repromote != -1,
        "covered clear block (store 0 / clear_eval_force_slots / repromote bump) intact",
    )
    report(
        "AC3.no_rearm_needed",
        no_rearm_in_clear,
        "FallBackJit path carries no re-arm (covered clear always changes the face)",
    )
    report(
        "AC3.doc",
        "Issue #4147" in cpp_raw[max(0, fn_raw - 3000) : fn_raw] if fn_raw != -1 else False,
        "function doc comment states the #4147 re-arm contract",
    )

    # AC4 — Soft / Off zero-cost preserved (probe early-return first).
    probe = win.find("aura_production_defaults_active_probe() == 0")
    report(
        "AC4.soft_skip",
        probe != -1 and probe < gate if gate != -1 else probe != -1,
        "production_defaults probe early-return stays the first check",
    )

    # AC5 — runtime doors; no invented test file / doc / query key.
    runners = (
        "ac4147_noop_heal_rearms_cap",
        "ac4147_window_gate_after_noop_heal",
        "ac4147_shrunk_residual_new_generation_heals",
        "ac4147_soft_zero_cost",
    )
    for r in runners:
        defined = f"static void {r}(CompilerService& cs)" in tst3096
        dispatched = tst3096.count(f"{r}(cs);") == 1
        report(
            f"AC5.{r}",
            defined and dispatched,
            "runner defined and dispatched exactly once in run_test_issue_3096",
        )
    report(
        "AC5.restated_ac3",
        "no-op heal cleared residual_force_auto_heal_last_mask" in tst3096 and "Issue #4147" in tst3096,
        "test_issue_3096.cpp cites #4147 with the restated AC3 fixture",
    )
    report(
        "AC5.ac3248_restated",
        "#4147" in tstrec and "heal1 + 1" in tstrec,
        "test_reload_recovery_query.cpp ac3248_1 restated for the re-arm contract",
    )
    no_test_file = not (ROOT / "tests" / "compiler" / "test_issue_4147.cpp").exists()
    no_doc = not any((ROOT / "docs" / "design").glob("4147-*")) if (ROOT / "docs" / "design").exists() else True
    report(
        "AC5.no_invent",
        no_test_file and no_doc,
        "no tests/compiler/test_issue_4147.cpp; no docs/design/4147-*",
    )
    report(
        "AC5.no_query_key",
        "schema-4147" not in cpp_raw,
        "no new query key (existing counters reused)",
    )

    # AC6 — wiring.
    report(
        "AC6.wiring",
        "check_residual_force_rearm_4147.py" in build and "check_residual_force_rearm_4147.py" in allow,
        "build.py wires this linter; root_check_allowlist.txt lists it",
    )

    print(f"check_residual_force_rearm_4147: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
